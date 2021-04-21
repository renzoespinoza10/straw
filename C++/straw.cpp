/*
  The MIT License (MIT)

  Copyright (c) 2011-2016 Broad Institute, Aiden Lab

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <cmath>
#include <set>
#include <vector>
#include <streambuf>
#include <curl/curl.h>
#include "zlib.h"
#include "straw.h"
using namespace std;

/*
  Straw: fast C++ implementation of dump. Not as fully featured as the
  Java version. Reads the .hic file, finds the appropriate matrix and slice
  of data, and outputs as text in sparse upper triangular format.

  Currently only supporting matrices.

  Usage: straw <NONE/VC/VC_SQRT/KR/OE> <hicFile(s)> <chr1>[:x1:x2] <chr2>[:y1:y2] <BP/FRAG> <binsize>
 */
// this is for creating a stream from a byte array for ease of use
struct membuf : std::streambuf {
    membuf(char *begin, char *end) {
        this->setg(begin, begin, end);
    }
};

// for holding data from URL call
struct MemoryStruct {
    char *memory;
    size_t size;
};

// version number
int version;

// map of block numbers to pointers


long total_bytes;

size_t hdf(char* b, size_t size, size_t nitems, void *userdata) {
    size_t numbytes = size * nitems;
    b[numbytes + 1] = '\0';
    string s(b);
    int found = s.find("Content-Range");
    if (found != string::npos) {
        int found2 = s.find("/");
        //Content-Range: bytes 0-100000/891471462
        if (found2 != string::npos) {
            string total = s.substr(found2 + 1);
            total_bytes = stol(total);
        }
    }

    return numbytes;
}

// callback for libcurl. data written to this buffer
static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *) userp;

    mem->memory = static_cast<char *>(realloc(mem->memory, mem->size + realsize + 1));
    if (mem->memory == NULL) {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    std::memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// get a buffer that can be used as an input stream from the URL
char *getData(CURL *curl, long position, long chunksize) {
    std::ostringstream oss;
    struct MemoryStruct chunk;

    chunk.memory = static_cast<char *>(malloc(1));
    chunk.size = 0;    /* no data at this point */
    oss << position << "-" << position + chunksize;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &chunk);
    curl_easy_setopt(curl, CURLOPT_RANGE, oss.str().c_str());
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n",
                curl_easy_strerror(res));
    }
    //  printf("%lu bytes retrieved\n", (long)chunk.size);

    return chunk.memory;
}

// initialize the CURL stream
CURL* initCURL(const char* url) {
    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        //curl_easy_setopt (curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, hdf);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "straw");
    }
    return curl;
}

// returns whether or not this is valid HiC file
bool readMagicString(istream &fin) {
    string str;
    getline(fin, str, '\0');
    return str[0] == 'H' && str[1] == 'I' && str[2] == 'C';
}

char readCharFromFile(istream &fin) {
    char tempChar;
    fin.read((char *) &tempChar, sizeof(char));
    return tempChar;
}

short readShortFromFile(istream &fin) {
    short tempShort;
    fin.read((char *) &tempShort, sizeof(short));
    return tempShort;
}

int readIntFromFile(istream &fin) {
    int tempInt;
    fin.read((char *) &tempInt, sizeof(int));
    return tempInt;
}

long readLongFromFile(istream &fin) {
    long tempLong;
    fin.read((char *) &tempLong, sizeof(long));
    return tempLong;
}

float readFloatFromFile(istream &fin) {
    float tempFloat;
    fin.read((char *) &tempFloat, sizeof(float));
    return tempFloat;
}

double readDoubleFromFile(istream &fin) {
    double tempDouble;
    fin.read((char *) &tempDouble, sizeof(double));
    return tempDouble;
}

// reads the header, storing the positions of the normalization vectors and returning the masterIndexPosition pointer
map<string, chromosome> readHeader(istream &fin, long &masterIndexPosition) {
    map<string, chromosome> chromosomeMap;
    if (!readMagicString(fin)) {
        cerr << "Hi-C magic string is missing, does not appear to be a hic file" << endl;
        masterIndexPosition = -1;
        return chromosomeMap;
    }

    fin.read((char *) &version, sizeof(int));
    if (version < 6) {
        cerr << "Version " << version << " no longer supported" << endl;
        masterIndexPosition = -1;
        return chromosomeMap;
    }
    fin.read((char *) &masterIndexPosition, sizeof(long));
    string genomeID;
    getline(fin, genomeID, '\0');

    if (version > 8) {
        long nviPosition = readLongFromFile(fin);
        long nviLength = readLongFromFile(fin);
    }

    int nattributes = readIntFromFile(fin);

    // reading and ignoring attribute-value dictionary
    for (int i = 0; i < nattributes; i++) {
        string key, value;
        getline(fin, key, '\0');
        getline(fin, value, '\0');
    }
    int nChrs = readIntFromFile(fin);
    // chromosome map for finding matrix
    for (int i = 0; i < nChrs; i++) {
        string name;
        long length;
        getline(fin, name, '\0');
        if (version > 8) {
            fin.read((char *) &length, sizeof(long));
        } else {
            length = (long) readIntFromFile(fin);
        }

        chromosome chr;
        chr.index = i;
        chr.name = name;
        chr.length = length;
        chromosomeMap[name] = chr;
    }
    return chromosomeMap;
}

// reads the footer from the master pointer location. takes in the chromosomes,
// norm, unit (BP or FRAG) and resolution or binsize, and sets the file
// position of the matrix and the normalization vectors for those chromosomes
// at the given normalization and resolution
bool readFooter(istream &fin, long master, int c1, int c2, string matrix, string norm, string unit, int resolution,
                long &myFilePos, indexEntry &c1NormEntry, indexEntry &c2NormEntry, vector<double> &expectedValues) {
    if (version > 8) {
        long nBytes = readLongFromFile(fin);
    } else {
        int nBytes = readIntFromFile(fin);
    }

    stringstream ss;
    ss << c1 << "_" << c2;
    string key = ss.str();

    int nEntries = readIntFromFile(fin);
    bool found = false;
    for (int i = 0; i < nEntries; i++) {
        string str;
        getline(fin, str, '\0');
        long fpos = readLongFromFile(fin);
        int sizeinbytes = readIntFromFile(fin);
        if (str == key) {
            myFilePos = fpos;
            found = true;
        }
    }
    if (!found) {
        cerr << "File doesn't have the given chr_chr map " << key << endl;
        return false;
    }

    if ((matrix=="observed" && norm=="NONE") || (matrix=="oe" && norm=="NONE" && c1!=c2)) return true; // no need to read norm vector index

    // read in and ignore expected value maps; don't store; reading these to
    // get to norm vector index
    int nExpectedValues = readIntFromFile(fin);
    for (int i = 0; i < nExpectedValues; i++) {
        string unit0;
        getline(fin, unit0, '\0'); //unit
        int binSize = readIntFromFile(fin);

        long nValues;
        if (version > 8) {
            fin.read((char *) &nValues, sizeof(long));
        } else {
            nValues = (long) readIntFromFile(fin);
        }

        bool store = c1 == c2 && matrix == "oe" && norm == "NONE" && unit0 == unit && binSize == resolution;

        if (version > 8) {
            for (long j = 0; j < nValues; j++) {
                double v = (double) readFloatFromFile(fin);
                if (store) {
                    expectedValues.push_back(v);
                }
            }
        } else {
            for (int j = 0; j < nValues; j++) {
                double v = readDoubleFromFile(fin);
                if (store) {
                    expectedValues.push_back(v);
                }
            }
        }

        int nNormalizationFactors = readIntFromFile(fin);
        for (int j = 0; j < nNormalizationFactors; j++) {
            int chrIdx = readIntFromFile(fin);
            double v;
            if (version > 8) {
                readFloatFromFile(fin);
            } else {
                readDoubleFromFile(fin);
            }
            if (store && chrIdx == c1) {
                for (vector<double>::iterator it=expectedValues.begin(); it!=expectedValues.end(); ++it) {
                    *it = *it / v;
                }
            }
        }
    }

    if (c1 == c2 && matrix == "oe" && norm == "NONE") {
        if (expectedValues.size() == 0) {
            cerr << "File did not contain expected values vectors at " << resolution << " " << unit << endl;
            return false;
        }
        return true;
    }

    nExpectedValues = readIntFromFile(fin);
    for (int i = 0; i < nExpectedValues; i++) {
        string type, unit0;
        getline(fin, type, '\0'); //typeString
        getline(fin, unit0, '\0'); //unit
        int binSize = readIntFromFile(fin);

        long nValues;
        if (version > 8) {
            fin.read((char *) &nValues, sizeof(long));
        } else {
            nValues = (long) readIntFromFile(fin);
        }
        bool store = c1 == c2 && matrix == "oe" && type == norm && unit0 == unit && binSize == resolution;

        if (version > 8) {
            for (long j = 0; j < nValues; j++) {
                double v = (double) readFloatFromFile(fin);
                if (store) {
                    expectedValues.push_back(v);
                }
            }
        } else {
            for (int j = 0; j < nValues; j++) {
                double v = readDoubleFromFile(fin);
                if (store) {
                    expectedValues.push_back(v);
                }
            }

        }

        int nNormalizationFactors = readIntFromFile(fin);
        for (int j = 0; j < nNormalizationFactors; j++) {
            int chrIdx = readIntFromFile(fin);
            double v;
            if (version > 8) {
                v = (double) readFloatFromFile(fin);
            } else {
                v = readDoubleFromFile(fin);
            }
            if (store && chrIdx == c1) {
                for (vector<double>::iterator it=expectedValues.begin(); it!=expectedValues.end(); ++it) {
                    *it = *it / v;
                }
            }
        }
    }

    if (c1 == c2 && matrix == "oe" && norm != "NONE") {
        if (expectedValues.size() == 0) {
            cerr << "File did not contain normalized expected values vectors at " << resolution << " " << unit << endl;
            return false;
        }
    }

    // Index of normalization vectors
    nEntries = readIntFromFile(fin);
    bool found1 = false;
    bool found2 = false;
    for (int i = 0; i < nEntries; i++) {
        string normtype;
        getline(fin, normtype, '\0'); //normalization type
        int chrIdx = readIntFromFile(fin);
        string unit1;
        getline(fin, unit1, '\0'); //unit
        int resolution1 = readIntFromFile(fin);
        long filePosition = readLongFromFile(fin);
        long sizeInBytes;
        if (version > 8) {
            fin.read((char *) &sizeInBytes, sizeof(long));
        } else {
            sizeInBytes = (long) readIntFromFile(fin);
        }

        if (chrIdx == c1 && normtype == norm && unit1 == unit && resolution1 == resolution) {
            c1NormEntry.position = filePosition;
            c1NormEntry.size = sizeInBytes;
            found1 = true;
        }
        if (chrIdx == c2 && normtype == norm && unit1 == unit && resolution1 == resolution) {
            c2NormEntry.position = filePosition;
            c2NormEntry.size = sizeInBytes;
            found2 = true;
        }
    }
    if (!found1 || !found2) {
        cerr << "File did not contain " << norm << " normalization vectors for one or both chromosomes at "
             << resolution << " " << unit << endl;
    }
    return true;
}


// reads the raw binned contact matrix at specified resolution, setting the block bin count and block column count
map <int, indexEntry> readMatrixZoomData(istream& fin, string myunit, int mybinsize, float &mySumCounts, int &myBlockBinCount, int &myBlockColumnCount, bool &found) {
    
    map<int, indexEntry> blockMap;
    string unit;
    getline(fin, unit, '\0'); // unit
    readIntFromFile(fin); // Old "zoom" index -- not used
    float sumCounts = readFloatFromFile(fin); // sumCounts
    readFloatFromFile(fin); // occupiedCellCount
    readFloatFromFile(fin); // stdDev
    readFloatFromFile(fin); // percent95
    int binSize = readIntFromFile(fin);
    int blockBinCount = readIntFromFile(fin);
    int blockColumnCount = readIntFromFile(fin);

    found = false;
    if (myunit == unit && mybinsize == binSize) {
        mySumCounts = sumCounts;
        myBlockBinCount = blockBinCount;
        myBlockColumnCount = blockColumnCount;
        found = true;
    }

    int nBlocks = readIntFromFile(fin);

    for (int b = 0; b < nBlocks; b++) {
        int blockNumber = readIntFromFile(fin);
        long filePosition = readLongFromFile(fin);
        int blockSizeInBytes = readIntFromFile(fin);
        indexEntry entry;
        entry.size = (long) blockSizeInBytes;
        entry.position = filePosition;
        if (found) blockMap[blockNumber] = entry;
    }
    return blockMap;
}

// reads the raw binned contact matrix at specified resolution, setting the block bin count and block column count
map <int, indexEntry> readMatrixZoomDataHttp(CURL* curl, long &myFilePosition, string myunit, int mybinsize, float &mySumCounts, int &myBlockBinCount, int &myBlockColumnCount, bool &found) {

    map<int, indexEntry> blockMap;
    char *buffer;
    int header_size = 5 * sizeof(int) + 4 * sizeof(float);
    char *first;
    first = getData(curl, myFilePosition, 1);
    if (first[0] == 'B') {
        header_size += 3;
    } else if (first[0] == 'F') {
        header_size += 5;
    } else {
        cerr << "Unit not understood" << endl;
        return blockMap;
    }
    buffer = getData(curl, myFilePosition, header_size);
    membuf sbuf(buffer, buffer + header_size);
    istream fin(&sbuf);

    string unit;
    getline(fin, unit, '\0'); // unit
    readIntFromFile(fin); // Old "zoom" index -- not used
    float sumCounts = readFloatFromFile(fin); // sumCounts
    readFloatFromFile(fin); // occupiedCellCount
    readFloatFromFile(fin); // stdDev
    readFloatFromFile(fin); // percent95
    int binSize = readIntFromFile(fin);
    int blockBinCount = readIntFromFile(fin);
    int blockColumnCount = readIntFromFile(fin);

    found = false;
    if (myunit == unit && mybinsize == binSize) {
        mySumCounts = sumCounts;
        myBlockBinCount = blockBinCount;
        myBlockColumnCount = blockColumnCount;
        found = true;
    }

    int nBlocks = readIntFromFile(fin);

    if (found) {
        buffer = getData(curl, myFilePosition + header_size, nBlocks * (sizeof(int) + sizeof(long) + sizeof(int)));
        membuf sbuf2(buffer, buffer + nBlocks * (sizeof(int) + sizeof(long) + sizeof(int)));
        istream fin2(&sbuf2);
        for (int b = 0; b < nBlocks; b++) {
            int blockNumber = readIntFromFile(fin2);
            long filePosition = readLongFromFile(fin2);
            int blockSizeInBytes = readIntFromFile(fin2);
            indexEntry entry;
            entry.size = (long) blockSizeInBytes;
            entry.position = filePosition;
            blockMap[blockNumber] = entry;
        }
    } else {
        myFilePosition = myFilePosition + header_size + (nBlocks * (sizeof(int) + sizeof(long) + sizeof(int)));
    }
    delete buffer;
    return blockMap;
}

// goes to the specified file pointer in http and finds the raw contact matrix at specified resolution, calling readMatrixZoomData.
// sets blockbincount and blockcolumncount
map <int, indexEntry> readMatrixHttp(CURL *curl, long myFilePosition, string unit, int resolution, float &mySumCounts, int &myBlockBinCount, int &myBlockColumnCount) {
    char *buffer;
    int size = sizeof(int) * 3;
    buffer = getData(curl, myFilePosition, size);
    membuf sbuf(buffer, buffer + size);
    istream bufin(&sbuf);

    int c1 = readIntFromFile(bufin);
    int c2 = readIntFromFile(bufin);
    int nRes = readIntFromFile(bufin);
    int i = 0;
    bool found = false;
    myFilePosition = myFilePosition + size;
    delete buffer;
    map<int, indexEntry> blockMap;

    while (i < nRes && !found) {
        // myFilePosition gets updated within call
        blockMap = readMatrixZoomDataHttp(curl, myFilePosition, unit, resolution, mySumCounts, myBlockBinCount, myBlockColumnCount,
                                          found);
        i++;
    }
    if (!found) {
        cerr << "Error finding block data" << endl;
    }
    return blockMap;
}

// goes to the specified file pointer and finds the raw contact matrix at specified resolution, calling readMatrixZoomData.
// sets blockbincount and blockcolumncount
map <int, indexEntry> readMatrix(istream& fin, long myFilePosition, string unit, int resolution, float &mySumCounts, int &myBlockBinCount, int &myBlockColumnCount) {
    map<int, indexEntry> blockMap;

    fin.seekg(myFilePosition, ios::beg);
    int c1 = readIntFromFile(fin);
    int c2 = readIntFromFile(fin);
    int nRes = readIntFromFile(fin);
    int i = 0;
    bool found = false;
    while (i < nRes && !found) {
        blockMap = readMatrixZoomData(fin, unit, resolution, mySumCounts, myBlockBinCount, myBlockColumnCount, found);
        i++;
    }
    if (!found) {
        cerr << "Error finding block data" << endl;
    }
    return blockMap;
}

// gets the blocks that need to be read for this slice of the data.  needs blockbincount, blockcolumncount, and whether
// or not this is intrachromosomal.
set<int> getBlockNumbersForRegionFromBinPosition(long *regionIndices, int blockBinCount, int blockColumnCount, bool intra) {
    int col1 = regionIndices[0] / blockBinCount;
    int col2 = (regionIndices[1] + 1) / blockBinCount;
    int row1 = regionIndices[2] / blockBinCount;
    int row2 = (regionIndices[3] + 1) / blockBinCount;

    set<int> blocksSet;
    // first check the upper triangular matrix
    for (int r = row1; r <= row2; r++) {
        for (int c = col1; c <= col2; c++) {
            int blockNumber = r * blockColumnCount + c;
            blocksSet.insert(blockNumber);
        }
    }
    // check region part that overlaps with lower left triangle but only if intrachromosomal
    if (intra) {
        for (int r = col1; r <= col2; r++) {
            for (int c = row1; c <= row2; c++) {
                int blockNumber = r * blockColumnCount + c;
                blocksSet.insert(blockNumber);
            }
        }
    }
    return blocksSet;
}

set<int> getBlockNumbersForRegionFromBinPositionV9Intra(long *regionIndices, int blockBinCount, int blockColumnCount) {
    // regionIndices is binX1 binX2 binY1 binY2
    set<int> blocksSet;
    int translatedLowerPAD = (regionIndices[0] + regionIndices[2]) / 2 / blockBinCount;
    int translatedHigherPAD = (regionIndices[1] + regionIndices[3]) / 2 / blockBinCount + 1;
    int translatedNearerDepth = log2(1 + abs(regionIndices[0] - regionIndices[3]) / sqrt(2) / blockBinCount);
    int translatedFurtherDepth = log2(1 + abs(regionIndices[1] - regionIndices[2]) / sqrt(2) / blockBinCount);

    // because code above assume above diagonal; but we could be below diagonal
    int nearerDepth = min(translatedNearerDepth, translatedFurtherDepth);
    if ((regionIndices[0] > regionIndices[3] && regionIndices[1] < regionIndices[2]) ||
        (regionIndices[1] > regionIndices[2] && regionIndices[0] < regionIndices[3])) {
        nearerDepth = 0;
    }
    int furtherDepth = max(translatedNearerDepth, translatedFurtherDepth) + 1; // +1; integer divide rounds down

    for (int depth = nearerDepth; depth <= furtherDepth; depth++) {
        for (int pad = translatedLowerPAD; pad <= translatedHigherPAD; pad++) {
            int blockNumber = depth * blockColumnCount + pad;
            blocksSet.insert(blockNumber);
        }
    }

    return blocksSet;
}

// this is the meat of reading the data.  takes in the block number and returns the set of contact records corresponding to
// that block.  the block data is compressed and must be decompressed using the zlib library functions
vector<contactRecord> readBlock(istream &fin, CURL *curl, bool isHttp, indexEntry idx) {
    if (idx.size == 0) {
        vector<contactRecord> v;
        return v;
    }
    char *compressedBytes = new char[idx.size];
    char *uncompressedBytes = new char[idx.size * 10]; //biggest seen so far is 3

    if (isHttp) {
        compressedBytes = getData(curl, idx.position, idx.size);
    } else {
        fin.seekg(idx.position, ios::beg);
        fin.read(compressedBytes, idx.size);
    }
    // Decompress the block
    // zlib struct
    z_stream infstream;
    infstream.zalloc = Z_NULL;
    infstream.zfree = Z_NULL;
    infstream.opaque = Z_NULL;
    infstream.avail_in = (uLong) (idx.size); // size of input
    infstream.next_in = (Bytef *) compressedBytes; // input char array
    infstream.avail_out = (uLong) idx.size * 10; // size of output
    infstream.next_out = (Bytef *) uncompressedBytes; // output char array
    // the actual decompression work.
    inflateInit(&infstream);
    inflate(&infstream, Z_NO_FLUSH);
    inflateEnd(&infstream);
    int uncompressedSize = infstream.total_out;

    // create stream from buffer for ease of use
    membuf sbuf(uncompressedBytes, uncompressedBytes + uncompressedSize);
    istream bufferin(&sbuf);
    int nRecords = readIntFromFile(bufferin);
    vector<contactRecord> v(nRecords);
    // different versions have different specific formats
    if (version < 7) {
        for (int i = 0; i < nRecords; i++) {
            contactRecord record;
            record.binX = readIntFromFile(bufferin);
            record.binY = readIntFromFile(bufferin);
            record.counts = readFloatFromFile(bufferin);
            v[i] = record;
        }
    } else {
        int binXOffset = readIntFromFile(bufferin);
        int binYOffset = readIntFromFile(bufferin);
        bool useShort = readCharFromFile(bufferin) == 0; // yes this is opposite of usual

        bool useShortBinX = true;
        bool useShortBinY = true;
        if (version > 8) {
            useShortBinX = readCharFromFile(bufferin) == 0;
            useShortBinY = readCharFromFile(bufferin) == 0;
        }

        char type = readCharFromFile(bufferin);
        int index = 0;
        if (type == 1) {
            if (useShortBinX && useShortBinY) {
                short rowCount = readShortFromFile(bufferin);
                for (short i = 0; i < rowCount; i++) {
                    int binY = binYOffset + readShortFromFile(bufferin);
                    short colCount = readShortFromFile(bufferin);
                    for (short j = 0; j < colCount; j++) {
                        int binX = binXOffset + readShortFromFile(bufferin);
                        float counts;
                        if (useShort) {
                            counts = readShortFromFile(bufferin);
                        } else {
                            bufferin.read((char *) &counts, sizeof(float));
                        }
                        contactRecord record;
                        record.binX = binX;
                        record.binY = binY;
                        record.counts = counts;
                        v[index] = record;
                        index++;
                    }
                }
            } else if (useShortBinX && !useShortBinY) {
                int rowCount = readIntFromFile(bufferin);
                for (int i = 0; i < rowCount; i++) {
                    int binY = binYOffset + readIntFromFile(bufferin);
                    short colCount = readShortFromFile(bufferin);
                    for (short j = 0; j < colCount; j++) {
                        int binX = binXOffset + readShortFromFile(bufferin);
                        float counts;
                        if (useShort) {
                            counts = readShortFromFile(bufferin);
                        } else {
                            bufferin.read((char *) &counts, sizeof(float));
                        }
                        contactRecord record;
                        record.binX = binX;
                        record.binY = binY;
                        record.counts = counts;
                        v[index] = record;
                        index++;
                    }
                }
            } else if (!useShortBinX && useShortBinY) {
                short rowCount = readShortFromFile(bufferin);
                for (short i = 0; i < rowCount; i++) {
                    int binY = binYOffset + readShortFromFile(bufferin);
                    int colCount = readIntFromFile(bufferin);
                    for (int j = 0; j < colCount; j++) {
                        int binX = binXOffset + readIntFromFile(bufferin);
                        float counts;
                        if (useShort) {
                            counts = readShortFromFile(bufferin);
                        } else {
                            bufferin.read((char *) &counts, sizeof(float));
                        }
                        contactRecord record;
                        record.binX = binX;
                        record.binY = binY;
                        record.counts = counts;
                        v[index] = record;
                        index++;
                    }
                }
            } else {
                int rowCount = readIntFromFile(bufferin);
                for (int i = 0; i < rowCount; i++) {
                    int binY = binYOffset + readIntFromFile(bufferin);
                    int colCount = readIntFromFile(bufferin);
                    for (int j = 0; j < colCount; j++) {
                        int binX = binXOffset + readIntFromFile(bufferin);
                        float counts;
                        if (useShort) {
                            counts = readShortFromFile(bufferin);
                        } else {
                            bufferin.read((char *) &counts, sizeof(float));
                        }
                        contactRecord record;
                        record.binX = binX;
                        record.binY = binY;
                        record.counts = counts;
                        v[index] = record;
                        index++;
                    }
                }
            }
        } else if (type == 2) {
            int nPts = readIntFromFile(bufferin);
            short w = readShortFromFile(bufferin);

            for (int i = 0; i < nPts; i++) {
                //int idx = (p.y - binOffset2) * w + (p.x - binOffset1);
                int row = i / w;
                int col = i - row * w;
                int bin1 = binXOffset + col;
                int bin2 = binYOffset + row;

                float counts;
                if (useShort == 0) { // yes this is opposite of the usual
                    short c = readShortFromFile(bufferin);
                    if (c != -32768) {
                        contactRecord record;
                        record.binX = bin1;
                        record.binY = bin2;
                        record.counts = c;
                        v[index] = record;
                        index++;
                    }
                } else {
                    bufferin.read((char *) &counts, sizeof(float));
                    if (!isnan(counts)) {
                        contactRecord record;
                        record.binX = bin1;
                        record.binY = bin2;
                        record.counts = counts;
                        v[index] = record;
                        index++;
                    }
                }
            }
        }
    }
    delete[] compressedBytes;
    delete[] uncompressedBytes; // don't forget to delete your heap arrays in C++!
    return v;
}

// reads the normalization vector from the file at the specified location
vector<double> readNormalizationVector(istream& bufferin) {
    long nValues;
    if (version > 8) {
        bufferin.read((char *) &nValues, sizeof(long));
    } else {
        nValues = (long) readIntFromFile(bufferin);
    }

    vector<double> values((int) nValues);

    if (version > 8) {
        for (long i = 0; i < nValues; i++) {
            values[i] = (double) readFloatFromFile(bufferin);
        }
    } else {
        for (int i = 0; i < nValues; i++) {
            values[i] = readDoubleFromFile(bufferin);
        }
    }

    //  if (allNaN) return null;
    return values;
}

class HiCFile {
public:
    string prefix = "http"; // HTTP code
    bool isHttp = false;
    ifstream fin;
    CURL *curl;
    long master;
    map<string, chromosome> chromosomeMap;

    HiCFile(string fname) {

        // read header into buffer; 100K should be sufficient
        if (std::strncmp(fname.c_str(), prefix.c_str(), prefix.size()) == 0) {
            isHttp = true;
            char *buffer;
            curl = initCURL(fname.c_str());
            if (curl) {
                buffer = getData(curl, 0, 100000);
            } else {
                cerr << "URL " << fname << " cannot be opened for reading" << endl;
                exit(1);
            }
            membuf sbuf(buffer, buffer + 100000);
            istream bufin(&sbuf);
            chromosomeMap = readHeader(bufin, master);
            delete buffer;
        } else {
            fin.open(fname, fstream::in);
            if (!fin) {
                cerr << "File " << fname << " cannot be opened for reading" << endl;
                exit(2);
            }
            chromosomeMap = readHeader(fin, master);
        }
    }
};

vector<double> readNormalizationVectorFromFooter(HiCFile *hiCFile, indexEntry cNormEntry) {
    char *buffer;
    if (hiCFile->isHttp) {
        buffer = getData(hiCFile->curl, cNormEntry.position, cNormEntry.size);
    } else {
        buffer = new char[cNormEntry.size];
        hiCFile->fin.seekg(cNormEntry.position, ios::beg);
        hiCFile->fin.read(buffer, cNormEntry.size);
    }
    membuf sbuf3(buffer, buffer + cNormEntry.size);
    istream bufferin(&sbuf3);
    vector<double> cNorm = readNormalizationVector(bufferin);
    delete buffer;
    return cNorm;
}

class Footer {
public:
    indexEntry c1NormEntry, c2NormEntry;
    long myFilePos;
    vector<double> expectedValues;
    long bytes_to_read;
    bool foundFooter = false;
    vector<double> c1Norm;
    vector<double> c2Norm;
    int c1;
    int c2;
    string matrix;
    string norm;
    string unit;
    int resolution;
    int numBins1;
    int numBins2;

// hiCFile.isHttp, hiCFile.master
    Footer(HiCFile *hiCFile, string chr1, string chr2, string matrix, string norm, string unit, int resolution) {
        int c01 = hiCFile->chromosomeMap[chr1].index;
        int c02 = hiCFile->chromosomeMap[chr2].index;
        if (c01 <= c02) { // default is ok
            this->c1 = c01;
            this->c2 = c02;
            this->numBins1 = hiCFile->chromosomeMap[chr1].length / resolution;
            this->numBins2 = hiCFile->chromosomeMap[chr2].length / resolution;
        } else { // flip
            this->c1 = c02;
            this->c2 = c01;
            this->numBins1 = hiCFile->chromosomeMap[chr2].length / resolution;
            this->numBins2 = hiCFile->chromosomeMap[chr1].length / resolution;
        }

        this->matrix = matrix;
        this->norm = norm;
        this->unit = unit;
        this->resolution = resolution;
        bytes_to_read = total_bytes - hiCFile->master;

        if (hiCFile->isHttp) {
            char *buffer2;
            buffer2 = getData(hiCFile->curl, hiCFile->master, bytes_to_read);
            membuf sbuf2(buffer2, buffer2 + bytes_to_read);
            istream bufin2(&sbuf2);
            foundFooter = readFooter(bufin2, hiCFile->master, c1, c2, matrix, norm, unit, resolution, myFilePos,
                                     c1NormEntry, c2NormEntry, expectedValues);
            delete buffer2;
        } else {
            hiCFile->fin.seekg(hiCFile->master, ios::beg);
            foundFooter = readFooter(hiCFile->fin, hiCFile->master, c1, c2, matrix, norm, unit, resolution, myFilePos,
                                     c1NormEntry, c2NormEntry, expectedValues);
        }

        if (!foundFooter) {
            return;
        }

        if (norm != "NONE") {
            c1Norm = readNormalizationVectorFromFooter(hiCFile, c1NormEntry);
            if (c1 == c2) {
                c2Norm = c1Norm;
            } else {
                c2Norm = readNormalizationVectorFromFooter(hiCFile, c2NormEntry);
            }
        }
    }
};

void parsePositions(string chrLoc, string &chrom, long &pos1, long &pos2, map<string, chromosome> map) {
    string x, y;
    stringstream ss(chrLoc);
    getline(ss, chrom, ':');
    if (map.count(chrom) == 0) {
        cerr << chrom << " not found in the file." << endl;
        exit(6);
    }

    if (getline(ss, x, ':') && getline(ss, y, ':')) {
        pos1 = stol(x);
        pos2 = stol(y);
    } else {
        pos1 = 0;
        pos2 = map[chrom].length;
    }
}

class MatrixZoomData {
public:
    float sumCounts;
    int blockBinCount, blockColumnCount;
    map<int, indexEntry> blockMap;
    vector<contactRecord> records;
    double avgCount;
    bool isIntra;

    MatrixZoomData(HiCFile *hiCFile, Footer *footer, long regionIndices[4], long origRegionIndices[4]) {

        isIntra = footer->c1 == footer->c2;

        if (hiCFile->isHttp) {
            // readMatrix will assign blockBinCount and blockColumnCount
            blockMap = readMatrixHttp(hiCFile->curl, footer->myFilePos, footer->unit, footer->resolution, sumCounts,
                                      blockBinCount,
                                      blockColumnCount);
        } else {
            // readMatrix will assign blockBinCount and blockColumnCount
            blockMap = readMatrix(hiCFile->fin, footer->myFilePos, footer->unit, footer->resolution, sumCounts,
                                  blockBinCount,
                                  blockColumnCount);
        }

        if (!isIntra) {
            avgCount = (sumCounts / footer->numBins1) / footer->numBins2;   // <= trying to avoid overflows
        }

        set<int> blockNumbers;

        if (version > 8 && isIntra) {
            blockNumbers = getBlockNumbersForRegionFromBinPositionV9Intra(regionIndices, blockBinCount,
                                                                          blockColumnCount);
        } else {
            blockNumbers = getBlockNumbersForRegionFromBinPosition(regionIndices, blockBinCount, blockColumnCount,
                                                                   isIntra);
        }

        // getBlockIndices

        vector<contactRecord> tmp_records;
        for (set<int>::iterator it = blockNumbers.begin(); it != blockNumbers.end(); ++it) {
            // get contacts in this block
            tmp_records = readBlock(hiCFile->fin, hiCFile->curl, hiCFile->isHttp, blockMap[*it]);
            for (vector<contactRecord>::iterator it2 = tmp_records.begin(); it2 != tmp_records.end(); ++it2) {
                contactRecord rec = *it2;

                long x = rec.binX * footer->resolution;
                long y = rec.binY * footer->resolution;

                if ((x >= origRegionIndices[0] && x <= origRegionIndices[1] &&
                     y >= origRegionIndices[2] && y <= origRegionIndices[3]) ||
                    // or check regions that overlap with lower left
                    (isIntra && y >= origRegionIndices[0] && y <= origRegionIndices[1] && x >= origRegionIndices[2] &&
                     x <= origRegionIndices[3])) {

                    float c = rec.counts;
                    if (footer->norm != "NONE") {
                        c = c / (footer->c1Norm[rec.binX] * footer->c2Norm[rec.binY]);
                    }
                    if (footer->matrix == "oe") {
                        if (isIntra) {
                            c = c / footer->expectedValues[min(footer->expectedValues.size() - 1,
                                                               (size_t) floor(abs(y - x) / footer->resolution))];
                        } else {
                            c = c / avgCount;
                        }
                    }


                    contactRecord record;
                    record.binX = x;
                    record.binY = y;
                    record.counts = c;
                    records.push_back(record);
                }
            }
        }
    }
};

vector<contactRecord>
straw(string matrix, string norm, string fname, string chr1loc, string chr2loc, string unit, int binsize) {
    if (!(unit == "BP" || unit == "FRAG")) {
        cerr << "Norm specified incorrectly, must be one of <BP/FRAG>" << endl;
        cerr << "Usage: straw <NONE/VC/VC_SQRT/KR> <hicFile(s)> <chr1>[:x1:x2] <chr2>[:y1:y2] <BP/FRAG> <binsize>"
             << endl;
        vector<contactRecord> v;
        return v;
    }

    HiCFile *hiCFile = new HiCFile(fname);

    // parse chromosome positions

    string chr1, chr2;
    long c1pos1 = -100, c1pos2 = -100, c2pos1 = -100, c2pos2 = -100;

    parsePositions(chr1loc, chr1, c1pos1, c1pos2, hiCFile->chromosomeMap);
    parsePositions(chr2loc, chr2, c2pos1, c2pos2, hiCFile->chromosomeMap);

    // from header have size of chromosomes, set region to read

    long origRegionIndices[4]; // as given by user
    // reverse order if necessary
    if (hiCFile->chromosomeMap[chr1].index > hiCFile->chromosomeMap[chr2].index) {
        origRegionIndices[0] = c2pos1;
        origRegionIndices[1] = c2pos2;
        origRegionIndices[2] = c1pos1;
        origRegionIndices[3] = c1pos2;
    } else {
        origRegionIndices[0] = c1pos1;
        origRegionIndices[1] = c1pos2;
        origRegionIndices[2] = c2pos1;
        origRegionIndices[3] = c2pos2;
    }
    long regionIndices[4]; // used to find the blocks we need to access
    regionIndices[0] = origRegionIndices[0] / binsize;
    regionIndices[1] = origRegionIndices[1] / binsize;
    regionIndices[2] = origRegionIndices[2] / binsize;
    regionIndices[3] = origRegionIndices[3] / binsize;

    Footer *footer = new Footer(hiCFile, chr1, chr2, matrix, norm, unit, binsize);

    if (!footer->foundFooter) {
        vector<contactRecord> v;
        return v;
    }

    MatrixZoomData *matrixZoomData = new MatrixZoomData(hiCFile, footer, regionIndices, origRegionIndices);

    return matrixZoomData->records;
}