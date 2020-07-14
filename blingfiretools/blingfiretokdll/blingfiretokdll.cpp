#include "FAConfig.h"
#include "FALimits.h"
#include "FAUtf8Utils.h"
#include "FAImageDump.h"
#include "FAWbdConfKeeper.h"
#include "FALDB.h"
#include "FALexTools_t.h"
#include "FADictConfKeeper.h"
#include "FATokenSegmentationTools_1best_t.h"
#include "FATokenSegmentationTools_1best_bpe_t.h"

#include <algorithm>
#include <vector>
#include <string>
#include <sstream>
#include <mutex>
#include <assert.h>

/*
This library provides easy interface to sentence and word-breaking functionality
which can be used in C#, Python, Perl, etc.

Also the goal is to reduce dependecies and provide the same library available
on Windows, Linux, Mac OSX, etc.
*/


// defines g_dumpBlingFireTokLibWbdData, the sources of this data is SearchGold\deploy\builds\data\IndexGenData\ldbsrc\ldb\tp3\wbd
#include "BlingFireTokLibWbdData.cxx"

// defines g_dumpBlingFireTokLibSbdData, the sources of this data is SearchGold\deploy\builds\data\IndexGenData\ldbsrc\ldb\tp3\sbd
#include "BlingFireTokLibSbdData.cxx"

// version of this binary and the algo logic
#define BLINGFIRETOK_MAJOR_VERSION_NUM 6
#define BLINGFIRETOK_MINOR_VERSION_NUM 1

const int WBD_WORD_TAG = 1;
const int WBD_IGNORE_TAG = 4;

// flag indicating the one-time initialization is done
volatile bool g_fInitialized = false;
std::mutex g_InitializationMutex; // this mutex is used once for default models only

// keep model data together
struct FAModelData
{
    // image of the loaded file
    FAImageDump m_Img;
    FALDB m_Ldb;

    // data and const processor for tokenization
    FAWbdConfKeeper m_Conf;
    FALexTools_t < int > m_Engine;
    bool m_hasWbd;

    // data and const processor for tokenization
    FADictConfKeeper m_DictConf;
    FATokenSegmentationTools_1best_t < int > m_SegEngine;
    bool m_hasSeg;

    // BPE runtime, it uses the m_DictConf for data
    FATokenSegmentationTools_1best_bpe_t < int > m_SegEngineBpe;
    bool m_isBpe;

    FAModelData ():
        m_hasWbd (false),
        m_hasSeg (false),
        m_isBpe (false)
    {}
};

// keep two built-in models one for default WBD and one for default SBD 
FAModelData g_DefaultWbd;
FAModelData g_DefaultSbd;

//
// returns the current version of the algo
//
extern "C"
const int GetBlingFireTokVersion()
{
    return (BLINGFIRETOK_MAJOR_VERSION_NUM * 1000) + BLINGFIRETOK_MINOR_VERSION_NUM;
}


// does initialization
void InitializeWbdSbd()
{
    const int * pValues = NULL;
    int iSize = 0;

    // initialize WBD
    g_DefaultWbd.m_Ldb.SetImage(g_dumpBlingFireTokLibWbdData);
    pValues = NULL;
    iSize = g_DefaultWbd.m_Ldb.GetHeader()->Get(FAFsmConst::FUNC_WBD, &pValues);
    g_DefaultWbd.m_Conf.Initialize(&g_DefaultWbd.m_Ldb, pValues, iSize);
    g_DefaultWbd.m_Engine.SetConf(&g_DefaultWbd.m_Conf);

    // initialize SBD
    g_DefaultSbd.m_Ldb.SetImage(g_dumpBlingFireTokLibSbdData);
    pValues = NULL;
    iSize = g_DefaultSbd.m_Ldb.GetHeader()->Get(FAFsmConst::FUNC_WBD, &pValues);
    g_DefaultSbd.m_Conf.Initialize(&g_DefaultSbd.m_Ldb, pValues, iSize);
    g_DefaultSbd.m_Engine.SetConf(&g_DefaultSbd.m_Conf);
}

// SENTENCE PIECE DELIMITER
#define __FASpDelimiter__ 0x2581

// WHITESPACE [\x0004-\x0020\x007F-\x009F\x00A0\x2000-\x200F\x202F\x205F\x2060\x2420\x2424\x3000\xFEFF]
#define __FAIsWhiteSpace__(C) ( \
        (C <=  0x20 || C == 0xa0   || (C >= 0x2000 && C <= 0x200f) || \
        C == 0x202f || C == 0x205f || C == 0x2060 || C == 0x2420 || \
        C == 0x2424 || C == 0x3000 || C == 0xfeff) \
    )


inline int FAGetFirstNonWhiteSpace(int * pStr, const int StrLen)
{
    for (int i = 0; i < StrLen; ++i)
    {
        const int C = pStr[i];
        if (__FAIsWhiteSpace__(C)) {
            continue;
        }
        return i;
    }

    return StrLen;
}


//
// The same as TextToSentences, but it allows to use a custom model and returns offsets
// 
// pStartOffsets is an array of integers (first character of each sentence) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each sentence) with upto MaxOutUtf8StrByteCount elements
//
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//

extern "C"
const int TextToSentencesWithOffsetsWithModel(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, const int MaxOutUtf8StrByteCount,
    void * hModel)
{
    // check if the initilization is needed
    if (false == g_fInitialized) {
        // make sure only one thread can get the mutex
        std::lock_guard<std::mutex> guard(g_InitializationMutex);
        // see if the g_fInitialized is still false
        if (false == g_fInitialized) {
            InitializeWbdSbd();
            g_fInitialized = true;
        }
    }

    // use the default model if it was not provided
    if (NULL == hModel) {
        hModel = &g_DefaultSbd;
    }

    // get the types right, hModel is always defined 
    const FAModelData * pModel = (const FAModelData *) hModel;

    // validate the parameters
    if (0 == InUtf8StrByteCount) {
        return 0;
    }
    if (0 > InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize) {
        return -1;
    }
    if (NULL == pInUtf8Str) {
        return -1;
    }

    // allocate buffer for UTF-32, sentence breaking results, word-breaking results
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return -1;
    }
    std::vector< int > utf32offsets(InUtf8StrByteCount);
    int * pOffsets = utf32offsets.data();
    if (NULL == pOffsets) {
        return -1;
    }
    // make sure there are no uninitialized offsets
    if (pStartOffsets) {
        memset(pStartOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }
    if (pEndOffsets) {
        memset(pEndOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }

    // convert input to UTF-32
    const int MaxBuffSize = ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, pOffsets, InUtf8StrByteCount);
    if (MaxBuffSize <= 0 || MaxBuffSize > InUtf8StrByteCount) {
        return -1;
    }
    // make sure the utf32input does not contain 'U+0000' elements
    std::replace(pBuff, pBuff + MaxBuffSize, 0, 0x20);

    // allocated a buffer for UTF-8 output
    std::vector< char > utf8output(InUtf8StrByteCount + 1);
    char * pTmpUtf8 = utf8output.data();
    if (NULL == pTmpUtf8) {
        return -1;
    }

    // keep sentence boundary information here
    std::vector< int > SbdRes(MaxBuffSize * 3);
    int * pSbdRes = SbdRes.data();
    if (NULL == pSbdRes) {
        return -1;
    }

    // get the sentence breaking results
    const int SbdOutSize = pModel->m_Engine.Process(pBuff, MaxBuffSize, pSbdRes, MaxBuffSize * 3);
    if (SbdOutSize > MaxBuffSize * 3 || 0 != SbdOutSize % 3) {
        return -1;
    }

    // number of sentences
    int SentCount = 0;
    // accumulate the output here
    std::ostringstream Os;
    // keep track if a sentence was already added
    bool fAdded = false;
    // set previous sentence end to -1
    int PrevEnd = -1;

    for (int i = 0; i < SbdOutSize; i += 3) {

        // we don't care about Tag or From for p2s task
        const int From = PrevEnd + 1;
        const int To = pSbdRes[i + 2];
        const int Len = To - From + 1;
        PrevEnd = To;

        // adjust sentence start if needed
        const int Delta = FAGetFirstNonWhiteSpace(pBuff + From, Len);
        if (Delta < Len) {
            // convert buffer to a UTF-8 string, we temporary use pOutUtf8Str, MaxOutUtf8StrByteCount
            const int StrOutSize = ::FAArrayToStrUtf8(pBuff + From + Delta, Len - Delta, pTmpUtf8, InUtf8StrByteCount);
            if (pStartOffsets && SentCount < MaxOutUtf8StrByteCount) {
                pStartOffsets[SentCount] = pOffsets[From + Delta];
            }
            if (pEndOffsets && SentCount < MaxOutUtf8StrByteCount) {
                const int ToCharSize = ::FAUtf8Size(pInUtf8Str + pOffsets[To]);
                pEndOffsets[SentCount] = pOffsets[To] + (0 < ToCharSize ? ToCharSize - 1 : 0);
            }
            SentCount++;

            // check the output size
            if (0 > StrOutSize || StrOutSize > InUtf8StrByteCount) {
                // should never happen, but happened :-(
                return -1;
            }
            else {
                // add a new line separator
                if (fAdded) {
                    Os << '\n';
                }
                // make sure this buffer does not contain '\n' since it is a delimiter
                std::replace(pTmpUtf8, pTmpUtf8 + StrOutSize, '\n', ' ');
                // actually copy the data into the string builder
                pTmpUtf8[StrOutSize] = 0;
                Os << pTmpUtf8;
                fAdded = true;
            }
        }
    }

    // always use the end of paragraph as the end of sentence
    if (PrevEnd + 1 < MaxBuffSize) {

        const int From = PrevEnd + 1;
        const int To = MaxBuffSize - 1;
        const int Len = To - From + 1;

        // adjust sentence start if needed
        const int Delta = FAGetFirstNonWhiteSpace(pBuff + From, Len);
        if (Delta < Len) {
            // convert buffer to a UTF-8 string, we temporary use pOutUtf8Str, MaxOutUtf8StrByteCount
            const int StrOutSize = ::FAArrayToStrUtf8(pBuff + From + Delta, Len - Delta, pTmpUtf8, InUtf8StrByteCount);
            if (pStartOffsets && SentCount < MaxOutUtf8StrByteCount) {
                pStartOffsets[SentCount] = pOffsets[From + Delta];
            }
            if (pEndOffsets && SentCount < MaxOutUtf8StrByteCount) {
                const int ToCharSize = ::FAUtf8Size(pInUtf8Str + pOffsets[To]);
                pEndOffsets[SentCount] = pOffsets[To] + (0 < ToCharSize ? ToCharSize - 1 : 0);
            }
            SentCount++;

            // check the output size
            if (0 > StrOutSize || StrOutSize > InUtf8StrByteCount) {
                // should never happen, but happened :-(
                return -1;
            }
            else {
                // add a new line separator
                if (fAdded) {
                    Os << '\n';
                }
                // make sure this buffer does not contain '\n' since it is a delimiter
                std::replace(pTmpUtf8, pTmpUtf8 + StrOutSize, '\n', ' ');
                // actually copy the data into the string builder
                pTmpUtf8[StrOutSize] = 0;
                Os << pTmpUtf8;
            }
        }
    }

    // we will include the 0 just in case some scriping languages expect 0-terminated buffers and cannot use the size
    Os << char(0);

    // get the actual output buffer as one string
    const std::string & OsStr = Os.str();
    const char * pStr = OsStr.c_str();
    const int StrLen = (int)OsStr.length();

    if (StrLen <= MaxOutUtf8StrByteCount) {
        memcpy(pOutUtf8Str, pStr, StrLen);
    }
    return StrLen;
}


//
// The same as TextToSentences, but this one also returns original offsets from the input buffer for each sentence.
//
// pStartOffsets is an array of integers (first character of each sentence) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each sentence) with upto MaxOutUtf8StrByteCount elements
//
extern "C"
const int TextToSentencesWithOffsets(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, const int MaxOutUtf8StrByteCount)
{
    return TextToSentencesWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str,  pStartOffsets, pEndOffsets, MaxOutUtf8StrByteCount, NULL);
}


//
// The same as TextToSentences, but allows to load a custom model
// 
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//
extern "C"
const int TextToSentencesWithModel(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, void * hModel)
{
    return TextToSentencesWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str,  NULL, NULL, MaxOutUtf8StrByteCount, hModel);
}


//
// Splits plain-text in UTF-8 encoding into sentences.
//
// Input:  UTF-8 string of one paragraph or document
// Output: Size in bytes of the output string and UTF-8 string of '\n' delimited sentences, if the return size <= MaxOutUtf8StrByteCount
//
//  Notes:  
//
//  1. The return value is -1 in case of an error (make sure your input is a valid UTF-8, BOM is not required)
//  2. We do not use "\r\n" to delimit sentences, it is always '\n'
//  3. It is not necessary to remove '\n' from the input paragraph, sentence breaking algorithm will take care of them
//  4. The output sentences will not contain '\n' in them
//
extern "C"
const int TextToSentences(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount)
{
    return TextToSentencesWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str,  NULL, NULL, MaxOutUtf8StrByteCount, NULL);
}


//
// Same as TextToWords, but also returns original offsets from the input buffer for each word and allows to use a 
//  custom model
//
// pStartOffsets is an array of integers (first character of each word) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each word) with upto MaxOutUtf8StrByteCount elements
//
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//
extern "C"
const int TextToWordsWithOffsetsWithModel(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, const int MaxOutUtf8StrByteCount,
    void * hModel)
{
    // check if the initilization is needed
    if (false == g_fInitialized) {
        // make sure only one thread can get the mutex
        std::lock_guard<std::mutex> guard(g_InitializationMutex);
        // see if the g_fInitialized is still false
        if (false == g_fInitialized) {
            InitializeWbdSbd();
            g_fInitialized = true;
        }
    }

    // use a default model if none was provided
    if (NULL == hModel) {
        hModel = &g_DefaultWbd;
    }

    // pModel is always initialized here
    const FAModelData * pModel = (const FAModelData *) hModel; 

    // validate the parameters
    if (0 == InUtf8StrByteCount) {
        return 0;
    }
    if (0 > InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize) {
        return -1;
    }
    if (NULL == pInUtf8Str) {
        return -1;
    }

    // allocate buffer for UTF-32, sentence breaking results, word-breaking results
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return -1;
    }

    std::vector< int > utf32offsets(InUtf8StrByteCount);
    int * pOffsets = utf32offsets.data();
    if (NULL == pOffsets) {
        return -1;
    }
    // make sure there are no uninitialized offsets
    if (pStartOffsets) {
        memset(pStartOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }
    if (pEndOffsets) {
        memset(pEndOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }

    // convert input to UTF-32
    const int MaxBuffSize = ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, pOffsets, InUtf8StrByteCount);
    if (MaxBuffSize <= 0 || MaxBuffSize > InUtf8StrByteCount) {
        return -1;
    }
    // make sure the utf32input does not contain 'U+0000' elements
    std::replace(pBuff, pBuff + MaxBuffSize, 0, 0x20);

    // allocated a buffer for UTF-8 output
    std::vector< char > utf8output(InUtf8StrByteCount + 1);
    char * pTmpUtf8 = utf8output.data();
    if (NULL == pTmpUtf8) {
        return -1;
    }

    // keep sentence boundary information here
    std::vector< int > WbdRes(MaxBuffSize * 3);
    int * pWbdRes = WbdRes.data();
    if (NULL == pWbdRes) {
        return -1;
    }

    // get the sentence breaking results
    const int WbdOutSize = pModel->m_Engine.Process(pBuff, MaxBuffSize, pWbdRes, MaxBuffSize * 3);
    if (WbdOutSize > MaxBuffSize * 3 || 0 != WbdOutSize % 3) {
        return -1;
    }

    // keep track of the word count
    int WordCount = 0;
    // accumulate the output here
    std::ostringstream Os;
    // keep track if a word was already added
    bool fAdded = false;

    for (int i = 0; i < WbdOutSize; i += 3) {

        // ignore tokens with IGNORE tag
        const int Tag = pWbdRes[i];
        if (WBD_IGNORE_TAG == Tag) {
            continue;
        }

        const int From = pWbdRes[i + 1];
        const int To = pWbdRes[i + 2];
        const int Len = To - From + 1;

        // convert buffer to a UTF-8 string, we temporary use pOutUtf8Str, MaxOutUtf8StrByteCount
        const int StrOutSize = ::FAArrayToStrUtf8(pBuff + From, Len, pTmpUtf8, InUtf8StrByteCount);
        if (pStartOffsets && WordCount < MaxOutUtf8StrByteCount) {
            pStartOffsets[WordCount] = pOffsets[From];
        }
        if (pEndOffsets && WordCount < MaxOutUtf8StrByteCount) {
            // offset of last UTF-32 character plus its length in bytes in the original string - 1
            const int ToCharSize = ::FAUtf8Size(pInUtf8Str + pOffsets[To]);
            pEndOffsets[WordCount] = pOffsets[To] + (0 < ToCharSize ? ToCharSize - 1 : 0);
        }
        WordCount++;

        // check the output size
        if (0 > StrOutSize || StrOutSize > InUtf8StrByteCount) {
            // should never happen, but happened :-(
            return -1;
        }
        else {
            // add a new line separator
            if (fAdded) {
                Os << ' ';
            }
            // make sure this buffer does not contain ' ' since it is a delimiter
            std::replace(pTmpUtf8, pTmpUtf8 + StrOutSize, ' ', '_');
            // actually copy the data into the string builder
            pTmpUtf8[StrOutSize] = 0;
            Os << pTmpUtf8;
            fAdded = true;
        }
    }

    // we will include the 0 just in case some scriping languages expect 0-terminated buffers and cannot use the size
    Os << char(0);

    // get the actual output buffer as one string
    const std::string & OsStr = Os.str();
    const char * pStr = OsStr.c_str();
    const int StrLen = (int)OsStr.length();

    if (StrLen <= MaxOutUtf8StrByteCount) {
        memcpy(pOutUtf8Str, pStr, StrLen);
    }
    return StrLen;
}


//
// Same as TextToWords, but also returns original offsets from the input buffer for each word.
//
// pStartOffsets is an array of integers (first character of each word) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each word) with upto MaxOutUtf8StrByteCount elements
//
extern "C"
const int TextToWordsWithOffsets(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, const int MaxOutUtf8StrByteCount)
{
    return TextToWordsWithOffsetsWithModel(pInUtf8Str,InUtf8StrByteCount,
        pOutUtf8Str, pStartOffsets, pEndOffsets, MaxOutUtf8StrByteCount, NULL);
}


//
// Same as TextToWords, but allows to load a custom model
// 
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//
extern "C"
const int TextToWordsWithModel(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, void * hModel)
{
    return TextToWordsWithOffsetsWithModel(pInUtf8Str,InUtf8StrByteCount,
        pOutUtf8Str, NULL, NULL, MaxOutUtf8StrByteCount, hModel);
}


//
// Splits plain-text in UTF-8 encoding into words.
//
// Input:  UTF-8 string of one sentence or paragraph or document
// Output: Size in bytes of the output string and UTF-8 string of ' ' delimited words, if the return size <= MaxOutUtf8StrByteCount
//
//  Notes:
//
//  1. The return value is -1 in case of an error (make sure your input is a valid UTF-8, BOM is not required)
//  2. Words from the word-breaker will not contain spaces
//
extern "C"
const int TextToWords(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount)
{
    return TextToWordsWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str, NULL, NULL, MaxOutUtf8StrByteCount, NULL);
}


//
// This function is like TextToWords, but it only normalizes consequtive spaces, it is not as flexble
//  as TextToWords as it cannot take a tokenization and normalization rules, but it does space normalization
//  and srinking multiple spaces to one faster.
//
// Input:  UTF-8 string of one sentence or paragraph or document
//         space character to use, default is __FASpDelimiter__ (U+x2581)
// Output: Size in bytes of the output string and UTF-8 string of ' ' delimited words, if the return size <= MaxOutUtf8StrByteCount
//
// Notes:
//  The return value is -1 in case of an error (make sure your input is a valid UTF-8, BOM is not required)
//  
extern "C"
const int NormalizeSpaces(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, const int uSpace = __FASpDelimiter__)
{
    // we can get weird results if this is not true
    DebugLogAssert(__FAIsWhiteSpace__(uSpace));

    if (0 == InUtf8StrByteCount) {
        return -1;
    }

    // allocate buffer for UTF-32, sentence breaking results, word-breaking results
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return -1;
    }

    // convert input to UTF-32
    int MaxBuffSize = ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, InUtf8StrByteCount);
    if (MaxBuffSize <= 0 || MaxBuffSize > InUtf8StrByteCount) {
        return -1;
    }

    int i = 0; // index for reading
    int j = 0; // index for writing
    while (i < MaxBuffSize) {

        const int Ci = pBuff[i++];

        // check if the Ci is not a space
        if (!__FAIsWhiteSpace__(Ci)) {
            // copy it
            pBuff[j++] = Ci;
        // if Ci is a space, check if the previous character was not a sapce
        } else if (0 < j && uSpace != pBuff[j - 1]) {
            // copy normalized space
            pBuff[j++] = uSpace;
        }
    } // of while ...

    // trim the final space if there was no content characters after
    if (1 < j && pBuff[j - 1] == uSpace) {
        j--;
    }

    MaxBuffSize = j;

    // convert UTF-32 back to UTF-8
    const int StrOutSize = ::FAArrayToStrUtf8(pBuff, MaxBuffSize, pOutUtf8Str, MaxOutUtf8StrByteCount);
    if (0 <= StrOutSize && StrOutSize < MaxOutUtf8StrByteCount) {
        pOutUtf8Str [StrOutSize] = 0;
    }

    return StrOutSize;
}


// This function implements the fasttext hashing function
inline const uint32_t GetHash(const char * str, size_t strLen)
{
    uint32_t h = 2166136261;
    for (size_t i = 0; i < strLen; i++) {
        h = h ^ uint32_t(int8_t(str[i]));
        h = h * 16777619;
    }
    return h;
}


// EOS symbol
int32_t EOS_HASH = GetHash("</s>", 4);

const void AddWordNgrams(int32_t * hashArray, int& hashCount, int32_t wordNgrams, int32_t bucket)
{
    // hash size
    const int tokenCount = hashCount;
    for (int32_t i = 0; i < tokenCount; i++) {
        uint64_t h = hashArray[i];
        for (int32_t j = i + 1; j < i + wordNgrams; j++) {
            uint64_t tempHash = (j < tokenCount) ? hashArray[j] : EOS_HASH;  // for computing ngram, we pad by EOS_HASH to allow each word has ngram which begins at itself.
            h = h * 116049371 + tempHash;
            hashArray[hashCount] = (h % bucket);
            ++hashCount;
        }
    }
}


// Get the tokens count by inspecting the spaces in input buffer
inline const int GetTokenCount(const char * input, const int bufferSize)
{
    if (bufferSize == 0)
    {
        return 0;
    }
    else
    {
        int spaceCount = 0;
        for (int i = 0; i < bufferSize; ++i)
        {
            if (input[i] == ' ')
            {
                spaceCount++;
            }
        }
        return spaceCount + 1;
    }
}


// Port the fast text getline function with modifications
// 1. do not have vocab
// 2. do not compute subwords info
const int ComputeHashes(const char * input, const int strLen, int32_t * hashArr, int wordNgrams, int bucketSize)
{
    int hashCount = 0;
    char * pTemp = (char *)input;
    char * pWordStart = &pTemp[0];
    size_t wordLength = 0;

    // add unigram hash first while reading the tokens. Unlike fasttext, there's no EOS padding here. 
    for (int pos = 0; pos < strLen; pos++)
    {
        if (pTemp[pos] == ' ' || pos == strLen - 1)  // if we hit word boundary pushback wordhash or end of sentence
        {
            uint32_t wordhash = GetHash(pWordStart, wordLength);
            hashArr[hashCount] = wordhash;
            ++hashCount;
            // move pointer to next word and reset length
            pWordStart = &pTemp[pos + 1];
            wordLength = 0;
        }
        else // otherwise keep moving temp pointer
        {
            ++wordLength;
        }
    }

    //Add higher order ngrams (bigrams and up), each token has a ngram starting from itself makes the size ngram * ntokens. 
    AddWordNgrams(hashArr, hashCount, wordNgrams, bucketSize);

    return hashCount;
}


// memory cap for stack memory allocation
const int MAX_ALLOCA_SIZE = 204800;


//
// This function implements the fasttext-like hashing logics. It assumes the input text is already tokenized and 
//  tokens are merged by a single space.
//
// Example: input :  "This is ok ."
//          output (bigram):  hash(this), hash(is), hash(ok), hash(.), hash(this is), hash(is ok), hash(ok .), hash(. EOS)
//
//  This is different from fasttext hashing:
//     1. no EOS padding by default for unigram.
//     2. do not take vocab as input, all unigrams are hashed too (we assume later caller repalces unigram hashes
//          with ids from a vocab file).
//
extern "C"
const int TextToHashes(const char * pInUtf8Str, int InUtf8StrByteCount, int32_t * pHashArr, const int MaxHashArrLength, int wordNgrams, int bucketSize = 2000000)
{
    // must have positive ngram
    if (wordNgrams <= 0 && InUtf8StrByteCount < 0)
    {
        return -1;
    }

    // get token count
    const int tokenCount = GetTokenCount(pInUtf8Str, InUtf8StrByteCount);

    // if memory allocation is not enough for current output, return requested memory amount
    if (tokenCount * wordNgrams >= MaxHashArrLength)
    {
        return InUtf8StrByteCount * wordNgrams;
    }

    // if correctly tokenized and the memory usage is within the limit
    const int hashArrSize = ComputeHashes(pInUtf8Str, InUtf8StrByteCount, pHashArr, wordNgrams, bucketSize);
    assert(hashArrSize == wordNgrams * tokenCount);

    return hashArrSize;
}


//
// Loads a model and return a handle.
// Returns 0 in case of an error.
//
extern "C"
void* LoadModel(const char * pszLdbFileName)
{
    FAModelData * pNewModelData = new FAModelData();
    if (NULL == pNewModelData) {
        return 0;
    }

    // load the bin file
    pNewModelData->m_Img.Load (pszLdbFileName);
    const unsigned char * pImgBytes = pNewModelData->m_Img.GetImageDump ();
    if (NULL == pImgBytes) {
        return 0;
    }

    // create a generic LDB object from bytes
    pNewModelData->m_Ldb.SetImage (pImgBytes);

    // get the configuration paramenters for [wbd]
    const int * pValues = NULL;
    int iSize = pNewModelData->m_Ldb.GetHeader ()->Get (FAFsmConst::FUNC_WBD, &pValues);

    // see if the [wbd] section is present
    if (-1 != iSize) {
        pNewModelData->m_hasWbd = true;
        // initialize WBD configuration
        pNewModelData->m_Conf.Initialize (&(pNewModelData->m_Ldb), pValues, iSize);
        // now initialize the engine
        pNewModelData->m_Engine.SetConf(&(pNewModelData->m_Conf));
    }

    // get the configuration paramenters for [pos-dict]
    pValues = NULL;
    iSize = pNewModelData->m_Ldb.GetHeader ()->Get (FAFsmConst::FUNC_POS_DICT, &pValues);

    // see if the [pos-dict] section is present
    if (-1 != iSize) {
        pNewModelData->m_hasSeg = true;
        // initialize dict configuration
        pNewModelData->m_DictConf.SetLDB (&(pNewModelData->m_Ldb));
        pNewModelData->m_DictConf.Init (pValues, iSize);

        // check if this is a Unigram LM or BPE model
        pNewModelData->m_isBpe = FAFsmConst::TOKENIZE_BPE == pNewModelData->m_DictConf.GetTokAlgo()
            || FAFsmConst::TOKENIZE_BPE_OPT == pNewModelData->m_DictConf.GetTokAlgo();

        // initialize the segmentation engine
        if (pNewModelData->m_isBpe)
        {
            pNewModelData->m_SegEngineBpe.SetConf(&pNewModelData->m_DictConf);
        } else {
            pNewModelData->m_SegEngine.SetConf(&pNewModelData->m_DictConf);
        }
    }

    return (void*) pNewModelData;
}


//
// Implements a word-piece algorithm. Returns ids of words or sub-words, returns upto MaxIdsArrLength ids,
// the rest of the array is unchanged, so the array can be set to initial length and fill with 0's for padding.
// If pStartOffsets and pEndOffsets are not NULL then fills in the start and end offset for each token.
// Return value is the number of ids copied into the array.
//
// Example:
//  input: Эpple pie.
//  fa_lex output: эpple/WORD э/WORD_ID_1208 pp/WORD_ID_9397 le/WORD_ID_2571 pie/WORD pie/WORD_ID_11345 ./WORD ./WORD_ID_1012
//  TextToIds output: [1208, 9397, 2571, 11345, 1012, ... <unchanged>]
//
extern "C"
const int TextToIdsWithOffsets_wp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr, 
        int * pStartOffsets, 
        int * pEndOffsets,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    // validate the parameters
    if (0 >= InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize || NULL == pInUtf8Str || 0 == ModelPtr) {
        return 0;
    }

    // allocate buffer for UTF-8 --> UTF-32 conversion
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return 0;
    }

    // a container for the offsets
    std::vector< int > utf32offsets;
    int * pOffsets = NULL;

    // flag to alter the logic in case we don't need the offsets
    const bool fNeedOffsets = NULL != pStartOffsets && NULL != pEndOffsets;

    if (fNeedOffsets) {
        utf32offsets.resize(InUtf8StrByteCount);
        pOffsets = utf32offsets.data();
        if (NULL == pOffsets) {
            return 0;
        }
    }

    // convert input to UTF-32, track offsets if needed
    int BuffSize = fNeedOffsets ? 
        ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, pOffsets, InUtf8StrByteCount) :
        ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, InUtf8StrByteCount);
    if (BuffSize <= 0 || BuffSize > InUtf8StrByteCount) {
        return 0;
    }

    // needed for normalization
    std::vector< int > utf32input_norm;
    int * pNormBuff = NULL;
    std::vector< int > utf32norm_offsets;
    int * pNormOffsets = NULL;

    // get the model data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;
    const FAWbdConfKeeper * pConf = &(pModelData->m_Conf);
    const FAMultiMapCA * pCharMap = pConf->GetCharMap ();

    // do the normalization for the entire input
    if (pCharMap) {

        utf32input_norm.resize(InUtf8StrByteCount);
        pNormBuff = utf32input_norm.data();
        if (NULL == pNormBuff) {
            return 0;
        }
        if (fNeedOffsets) {
            utf32norm_offsets.resize(InUtf8StrByteCount);
            pNormOffsets = utf32norm_offsets.data();
            if (NULL == pNormOffsets) {
                return 0;
            }
        }

        BuffSize = fNeedOffsets ? 
            ::FANormalize(pBuff, BuffSize, pNormBuff, pNormOffsets, InUtf8StrByteCount, pCharMap) :
            ::FANormalize(pBuff, BuffSize, pNormBuff, InUtf8StrByteCount, pCharMap);
        if (BuffSize <= 0 || BuffSize > InUtf8StrByteCount) {
            return 0;
        }

        // use normalized buffer as input
        pBuff = pNormBuff;
    }

    // keep sentence boundary information here
    const int WbdResMaxSize = BuffSize * 6;
    std::vector< int > WbdRes(WbdResMaxSize);
    int * pWbdRes = WbdRes.data();
    if (NULL == pWbdRes) {
        return 0;
    }

    // compute token and sub-token boundaries
    const int WbdOutSize = pModelData->m_Engine.Process(pBuff, BuffSize, pWbdRes, WbdResMaxSize);
    if (WbdOutSize > WbdResMaxSize || 0 != WbdOutSize % 3) {
        return 0;
    }

    int OutCount = 0;

    // iterate over the results
    for(int i = 0; i < WbdOutSize; i += 3) {

        // ignore tokens with IGNORE tag
        const int Tag = pWbdRes[i];
        if (WBD_IGNORE_TAG == Tag) {
            continue;
        }

        // For each token with WORD tag copy all subword tags into the output if
        //  this word is covered completely by the subwords without gaps,
        //  otherwise copy the UnkId tag (this is how it's done in the original BERT TokenizerFull).
        if (WBD_WORD_TAG == Tag) {

            const int TokenFrom = pWbdRes[i + 1];
            const int TokenTo = pWbdRes[i + 2];

            // see if we have subtokens for this token and they cover the token completely
            int j = i + 3;
            int numSubTokens = 0;
            bool subTokensCoveredAll = false;

            if (j < WbdOutSize) {

                int ExpectedFrom = TokenFrom;
                int SubTokenTag = pWbdRes[j];
                int SubTokenFrom = pWbdRes[j + 1];
                int SubTokenTo = pWbdRes[j + 2];

                // '<=' because last subtoken should be included
                while (j <= WbdOutSize && SubTokenTag > WBD_IGNORE_TAG && ExpectedFrom == SubTokenFrom) {

                    ExpectedFrom = SubTokenTo + 1;
                    numSubTokens++;
                    j += 3;
                    if (j < WbdOutSize) {
                        SubTokenTag = pWbdRes[j];
                        SubTokenFrom = pWbdRes[j + 1];
                        SubTokenTo = pWbdRes[j + 2];
                    } // else it will break at the while check
                }

                // if subtoken To is the same as token To then we split the token all the way
                if (0 < numSubTokens && ExpectedFrom - 1 == TokenTo) {
                    // output all subtokens tags
                    for(int k = 0; k < numSubTokens && OutCount < MaxIdsArrLength; ++k) {

                        const int TagIdx = ((k + 1) * 3) + i;
                        const int SubTokenTag = pWbdRes[TagIdx];

                        if (OutCount < MaxIdsArrLength) {

                            pIdsArr[OutCount] = SubTokenTag;

                            if (fNeedOffsets) {

                                const int SubTokenFrom = pWbdRes[TagIdx + 1];
                                const int FromOffset = pOffsets[(pCharMap) ? pNormOffsets [SubTokenFrom] : SubTokenFrom];
                                pStartOffsets[OutCount] = FromOffset;

                                const int SubTokenTo = pWbdRes[TagIdx + 2];
                                const int ToOffset = pOffsets[(pCharMap) ? pNormOffsets [SubTokenTo] : SubTokenTo];
                                const int ToCharSize = ::FAUtf8Size(pInUtf8Str + ToOffset);
                                pEndOffsets[OutCount] = ToOffset + (0 < ToCharSize ? ToCharSize - 1 : 0);
                            }

                            OutCount++;
                        }
                    }
                    subTokensCoveredAll = true;
                }
            }

            if (false == subTokensCoveredAll) {
                // output an unk tag
                if (OutCount < MaxIdsArrLength) {

                    pIdsArr[OutCount] = UnkId;

                    // for unknown tokens take offsets from the word
                    if (fNeedOffsets) {

                        const int FromOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenFrom] : TokenFrom];
                        pStartOffsets[OutCount] = FromOffset;

                        const int ToOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenTo] : TokenTo];
                        const int ToCharSize = ::FAUtf8Size(pInUtf8Str + ToOffset);
                        pEndOffsets[OutCount] = ToOffset + (0 < ToCharSize ? ToCharSize - 1 : 0);
                    }

                    OutCount++;
                }
            }

            // skip i forward if we looped over any subtokens
            i = (j - 3);

        } // of if (WBD_WORD_TAG == Tag) ...

        if (OutCount >= MaxIdsArrLength) {
            break;
        }
    }

    return OutCount;
}


//
// The same as TextToIdsWithOffsets_wp, except does not return offsets
//
extern "C"
const int TextToIds_wp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    return TextToIdsWithOffsets_wp(ModelPtr,pInUtf8Str,InUtf8StrByteCount,pIdsArr,NULL,NULL,MaxIdsArrLength,UnkId);
}


//
// Implements a sentence piece algorithm, returns predictions from FATokenSegmentationTools_1best_t.
// The input is always prepended with ' ' / '▁' since this seems the case in the sentence piece.
// Returns upto MaxIdsArrLength ids, the rest of the array is unchanged, so the array can be set to 
// initial length and fill with 0's for padding. Returns number of ids copied into the array.
//
// Example:
// printf "Sergei Alonichau I saw a girl with a \ttelescope." | spm_encode --model=xlnet/spiece.model 
// ▁Sergei ▁Al oni chau ▁I ▁saw ▁a ▁girl ▁with ▁a ▁telescope .
//
// printf "Sergei Alonichau I saw a girl with a \ttelescope." | spm_encode --model=xlnet/spiece.model --output_format=id
// 14363 651 7201 25263 35 685 24 1615 33 24 16163 9
//
// TextToIds_sp output: 12, [14363 651 7201 25263 35 685 24 1615 33 24 16163 9]
//
extern "C"
const int TextToIdsWithOffsets_sp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        int * pStartOffsets, 
        int * pEndOffsets,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    // validate the parameters
    if (0 >= InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize || NULL == pInUtf8Str || 0 == ModelPtr) {
        return 0;
    }

    // allocate buffer for UTF-8 --> UTF-32 conversion
    std::vector< int > utf32input(InUtf8StrByteCount + 1);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return 0;
    }
    pBuff[0] = __FASpDelimiter__; // always add a space in the beginning, SP uses U+2581 as a space mark

    // a container for the offsets
    std::vector< int > utf32offsets;
    int * pOffsets = NULL;

    // flag to alter the logic in case we don't need the offsets
    const bool fNeedOffsets = NULL != pStartOffsets && NULL != pEndOffsets;

    if (fNeedOffsets) {
        utf32offsets.resize(InUtf8StrByteCount + 1);
        pOffsets = utf32offsets.data();
        if (NULL == pOffsets) {
            return 0;
        }
        pOffsets[0] = 0; // added for prepended first character
    }

    // convert input to UTF-32 (write past the added first space)
    int BuffSize = fNeedOffsets ? 
        ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff + 1, pOffsets + 1, InUtf8StrByteCount) :
        ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff + 1, InUtf8StrByteCount);
    if (BuffSize <= 0 || BuffSize > InUtf8StrByteCount) {
        return 0;
    }
    BuffSize++; // to accomodate the first space

    // get the model data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;
    const FADictConfKeeper * pConf = &(pModelData->m_DictConf);
    const FAMultiMapCA * pCharMap = pConf->GetCharMap ();

    // needed for normalization
    std::vector< int > utf32input_norm;
    int * pNormBuff = NULL;
    std::vector< int > utf32norm_offsets;
    int * pNormOffsets = NULL;

    // do normalization, if needed
    if (NULL != pCharMap) {

        const int MaxNormBuffSize = (InUtf8StrByteCount + 1) * 2;
        utf32input_norm.resize(MaxNormBuffSize);
        pNormBuff = utf32input_norm.data();
        if (NULL == pNormBuff) {
            return 0;
        }
        if (fNeedOffsets) {
            utf32norm_offsets.resize(MaxNormBuffSize);
            pNormOffsets = utf32norm_offsets.data();
            if (NULL == pNormOffsets) {
                return 0;
            }
        }

        // do the normalization for the entire input
        const int ActualNormBuffSize = fNeedOffsets ? 
            ::FANormalize(pBuff, BuffSize, pNormBuff, pNormOffsets, MaxNormBuffSize, pCharMap) :
            ::FANormalize(pBuff, BuffSize, pNormBuff, MaxNormBuffSize, pCharMap);

        if (ActualNormBuffSize <= 0 || ActualNormBuffSize > MaxNormBuffSize) {
            pCharMap = NULL;
            // don't proceed without normalization, TODO: 99% times it does not change anything... so it is ok to proceed
            return 0;
        } else {
            BuffSize = ActualNormBuffSize;
            pBuff = pNormBuff;
        }
    }

    // Replace every space sequence with U+2581 in-place
    //
    // Note: This operation affect offsets. Since the output sequence is always the 
    //       same length or shorter we can update the offsets in-place.
    //       If normalization is enbled the offsets are computed as a superposition of
    //       normalization and utf-32 offsets, then transformation should be applied to 
    //       normalization offsets, otherwise to utf-32 offsets.
    //
    int * pAdjustedOffsets = fNeedOffsets ? (NULL != pCharMap ? pNormOffsets : pOffsets) : NULL;

    int i = 1; // index for reading
    int j = 1; // index for writing
    while (i < BuffSize) {

        const int Ci = pBuff[i];

        // check if the Ci is not a space
        if (!__FAIsWhiteSpace__(Ci)) {
            // copy it
            pBuff[j] = Ci;
            if (fNeedOffsets) {
                pAdjustedOffsets[j] = pAdjustedOffsets[i];
            }
            j++;
        // if Ci is a space, check if the previous character was not a sapce
        } else if (__FASpDelimiter__ != pBuff[j - 1]) {
            // copy normalized space
            pBuff[j] = __FASpDelimiter__;
            if (fNeedOffsets) {
                pAdjustedOffsets[j] = pAdjustedOffsets[i];
            }
            j++;
        }

        i++;

    } // of while ...

    // trim the final space if there was no content characters after
    if (1 < j && pBuff[j - 1] == __FASpDelimiter__) {
        j--;
    }

    // adjust the length
    BuffSize = j;

    // do the segmentation
    const int WbdResMaxSize = BuffSize * 3;
    std::vector< int > WbdResults(WbdResMaxSize);
    int * pWbdResults = WbdResults.data ();

    // use either unigram lm or bpe runtime
    const int WbdOutSize = pModelData->m_isBpe ? 
        pModelData->m_SegEngineBpe.Process (pBuff, BuffSize, pWbdResults, WbdResMaxSize, UnkId) :
        pModelData->m_SegEngine.Process (pBuff, BuffSize, pWbdResults, WbdResMaxSize, UnkId);
    if (WbdOutSize > WbdResMaxSize || 0 != WbdOutSize % 3) {
        return 0;
    }

    int OutSize = 0;

    // return the ids only
    for (int i = 0; i < WbdOutSize && OutSize < MaxIdsArrLength; i += 3) {

        // copy id
        const int id = pWbdResults [i];
        pIdsArr [OutSize] = id;

        // copy offsets if needed
        if (fNeedOffsets) {

            const int TokenFrom = pWbdResults [i + 1];
            const int FromOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenFrom] : TokenFrom];
            pStartOffsets[OutSize] = FromOffset;

            const int TokenTo = pWbdResults [i + 2];
            const int ToOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenTo] : TokenTo];
            const int ToCharSize = ::FAUtf8Size(pInUtf8Str + ToOffset);
            pEndOffsets[OutSize] = ToOffset + (0 < ToCharSize ? ToCharSize - 1 : 0);
        }

        OutSize++;
    }

    return OutSize;
}


//
// The same as TextToIdsWithOffsets_sp, except does not return offsets
//
extern "C"
const int TextToIds_sp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    return TextToIdsWithOffsets_sp(ModelPtr,pInUtf8Str,InUtf8StrByteCount,pIdsArr,NULL,NULL,MaxIdsArrLength,UnkId);
}


//
// Implements a word-piece or sentence piece algorithms which is defined by the loaded model. 
// Returns ids of words or sub-words, returns upto MaxIdsArrLength ids, the rest of the array 
// is unchanged, so the array can be set to initial length and fill with 0's for padding.
// Returns number of ids copied into the array.
//

extern "C"
const int TextToIdsWithOffsets(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        int * pStartOffsets, 
        int * pEndOffsets,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    if (0 == ModelPtr) {
        return 0;
    }

    // check if loaded model has segmentation data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;

    if (!pModelData->m_hasSeg)
    {
        // call word-piece algorithm
        return TextToIdsWithOffsets_wp(
                ModelPtr, 
                pInUtf8Str, 
                InUtf8StrByteCount, 
                pIdsArr, 
                pStartOffsets, 
                pEndOffsets, 
                MaxIdsArrLength, 
                UnkId
            );
    }
    else
    {
        // call sentence-piece algorithm
        return TextToIdsWithOffsets_sp(
                ModelPtr, 
                pInUtf8Str, 
                InUtf8StrByteCount, 
                pIdsArr, 
                pStartOffsets, 
                pEndOffsets, 
                MaxIdsArrLength, 
                UnkId
            );
    }
}


//
// Implements a word-piece or sentence piece algorithms which is defined by the loaded model. 
// Returns ids of words or sub-words, returns upto MaxIdsArrLength ids, the rest of the array 
// is unchanged, so the array can be set to initial length and fill with 0's for padding.
// Returns number of ids copied into the array.
//

extern "C"
const int TextToIds(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    if (0 == ModelPtr) {
        return 0;
    }

    // check if loaded model has segmentation data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;

    if (!pModelData->m_hasSeg)
    {
        // call word-piece algorithm
        return TextToIdsWithOffsets_wp(ModelPtr, pInUtf8Str, InUtf8StrByteCount, pIdsArr, NULL, NULL, MaxIdsArrLength, UnkId);
    }
    else
    {
        // call sentence-piece algorithm
        return TextToIdsWithOffsets_sp(ModelPtr, pInUtf8Str, InUtf8StrByteCount, pIdsArr, NULL, NULL, MaxIdsArrLength, UnkId);
    }
}


//
// Frees memory from the model, after this call ModelPtr is no longer valid
//  Double calls to this function with the same argument will case access violation
//
extern "C"
int FreeModel(void* ModelPtr)
{
    if (NULL == ModelPtr) {
        return 0;
    }

    delete (FAModelData*) ModelPtr;
    return 1;
}
