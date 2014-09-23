/*
 * PLEASE SEE LICENSE FILE FOR LICENSING AND COPYRIGHT INFORMATION
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "hash.h"
#include "hashtable.h"
#include "segments.h"
#include "sequencer.h"
#include "table.h"
#include "thread.h"
#include "utility.h"
#include "vector.h"
#include "tm_transition.h"
#include "types.h"

// [mfs] This is a hack
extern
__attribute__((transaction_pure))
int strncmp (__const char *__s1, __const char *__s2, size_t __n) throw();

struct endInfoEntry_t {
    bool isEnd;
    long jumpToNext;
};

/* =============================================================================
 * hashString
 * -- uses sdbm hash function
 * =============================================================================
 */
__attribute__((transaction_safe))
unsigned long
hashString (char* str)
{
    unsigned long hash = 0;
    long c;

    /* Note: Do not change this hashing scheme */
    while ((c = *str++) != '\0') {
        hash = c + (hash << 6) + (hash << 16) - hash;
    }

    return (unsigned long)hash;
}


/* =============================================================================
 * hashSegment
 * -- For hashtable
 * =============================================================================
 */
//[wer] need to be TM_SAFE
__attribute__((transaction_safe))
//__attibute__ ((transaction_pure))
unsigned long
hashSegment (const void* keyPtr)
{
  //return (unsigned long)hash_sdbm((char*)keyPtr); /* can be any "good" hash function */

  //[wer] I replaced hash_sdbm with the above sdbm function, which is TM_SAFE
  unsigned long hash = 0;
    long c;
    const char* str = (const char*)keyPtr;

    /* Note: Do not change this hashing scheme */
   while ((c = *str++) != '\0') {
        hash = c + (hash << 6) + (hash << 16) - hash;
    }

   return (unsigned long)hash;

}


//[wer] we need a safe version of strcmp
/*
 *  Compare S1 and S2, returning less than, equal to or
 *  greater than zero if S1 is lexicographically less than,
 *  equal to or greater than S2.
 */
__attribute__((transaction_safe))
inline long tm_safe_strcmp(const void* p1, const void* p2)
{

  register const unsigned char *s1 = (const unsigned char *) p1;
  register const unsigned char *s2 = (const unsigned char *) p2;

  unsigned char c1, c2;
  do
  {
      c1 = (unsigned char) *s1++;
      c2 = (unsigned char) *s2++;

      if (c1 == '\0')
        return c1 - c2;
  }while (c1 == c2);

  return c1 - c2;
}

__attribute__((transaction_pure))
inline static long tm_strcmp(void* a, void* b)
{
  return  strcmp((char*)a, (char*)b);
}


/* =============================================================================
 * compareSegment
 * -- For hashtable
 * =============================================================================
 */
__attribute__((transaction_safe))
long
compareSegment (const pair_t* a, const pair_t* b)
{
  void* aa = (void*)(a -> firstPtr);
  void* bb = (void*)(b -> firstPtr);
  //return tm_strcmp(aa, bb);
  return tm_safe_strcmp(aa, bb); //[wer210] use safe version is TOO slow for write-through alg.
}


/* =============================================================================
 * sequencer_alloc
 * -- Returns NULL on failure
 * =============================================================================
 */
sequencer_t*
sequencer_alloc (long geneLength, long segmentLength, segments_t* segmentsPtr)
{
    sequencer_t* sequencerPtr;
    long maxNumUniqueSegment = geneLength - segmentLength + 1;
    long i;

    sequencerPtr = (sequencer_t*)malloc(sizeof(sequencer_t));
    if (sequencerPtr == NULL) {
        return NULL;
    }

    sequencerPtr->uniqueSegmentsPtr =
        TMhashtable_alloc(geneLength, &hashSegment, &compareSegment, -1, -1);
    if (sequencerPtr->uniqueSegmentsPtr == NULL) {
        return NULL;
    }

    /* For finding a matching entry */
    sequencerPtr->endInfoEntries =
        (endInfoEntry_t*)malloc(maxNumUniqueSegment * sizeof(endInfoEntry_t));
    for (i = 0; i < maxNumUniqueSegment; i++) {
        endInfoEntry_t* endInfoEntryPtr = &sequencerPtr->endInfoEntries[i];
        endInfoEntryPtr->isEnd = true;
        endInfoEntryPtr->jumpToNext = 1;
    }
    sequencerPtr->startHashToConstructEntryTables =
        (table_t**)malloc(segmentLength * sizeof(table_t*));
    if (sequencerPtr->startHashToConstructEntryTables == NULL) {
        return NULL;
    }
    for (i = 1; i < segmentLength; i++) { /* 0 is dummy entry */
        sequencerPtr->startHashToConstructEntryTables[i] = new table_t(geneLength);
        if (sequencerPtr->startHashToConstructEntryTables[i] == NULL) {
            return NULL;
        }
    }
    sequencerPtr->segmentLength = segmentLength;

    /* For constructing sequence */
    sequencerPtr->constructEntries =
        (constructEntry_t*)malloc(maxNumUniqueSegment * sizeof(constructEntry_t));
    if (sequencerPtr->constructEntries == NULL) {
        return NULL;
    }
    for (i= 0; i < maxNumUniqueSegment; i++) {
        constructEntry_t* constructEntryPtr = &sequencerPtr->constructEntries[i];
        constructEntryPtr->isStart = true;
        constructEntryPtr->segment = NULL;
        constructEntryPtr->endHash = 0;
        constructEntryPtr->startPtr = constructEntryPtr;
        constructEntryPtr->nextPtr = NULL;
        constructEntryPtr->endPtr = constructEntryPtr;
        constructEntryPtr->overlap = 0;
        constructEntryPtr->length = segmentLength;
    }
    sequencerPtr->hashToConstructEntryTable = new table_t(geneLength);
    if (sequencerPtr->hashToConstructEntryTable == NULL) {
        return NULL;
    }

    sequencerPtr->segmentsPtr = segmentsPtr;

    return sequencerPtr;
}


/* =============================================================================
 * sequencer_run
 * =============================================================================
 */
void
sequencer_run (void* argPtr)
{
    long threadId = thread_getId();

    sequencer_t* sequencerPtr = (sequencer_t*)argPtr;

    hashtable_t*      uniqueSegmentsPtr;
    endInfoEntry_t*   endInfoEntries;
    table_t**         startHashToConstructEntryTables;
    constructEntry_t* constructEntries;
    table_t*          hashToConstructEntryTable;

    uniqueSegmentsPtr               = sequencerPtr->uniqueSegmentsPtr;
    endInfoEntries                  = sequencerPtr->endInfoEntries;
    startHashToConstructEntryTables = sequencerPtr->startHashToConstructEntryTables;
    constructEntries                = sequencerPtr->constructEntries;
    hashToConstructEntryTable       = sequencerPtr->hashToConstructEntryTable;

    segments_t* segmentsPtr         = sequencerPtr->segmentsPtr;
    assert(segmentsPtr);
    vector_t*   segmentsContentsPtr = segmentsPtr->contentsPtr;
    long        numSegment          = vector_getSize(segmentsContentsPtr);
    long        segmentLength       = segmentsPtr->length;

    long i;
    long j;
    long i_start;
    long i_stop;
    long numUniqueSegment;
    long substringLength;
    long entryIndex;

    /*
     * Step 1: Remove duplicate segments
     */
    long numThread = thread_getNumThread();
    {
        /* Choose disjoint segments [i_start,i_stop) for each thread */
        long partitionSize = (numSegment + numThread/2) / numThread; /* with rounding */
        i_start = threadId * partitionSize;
        if (threadId == (numThread - 1)) {
            i_stop = numSegment;
        } else {
            i_stop = i_start + partitionSize;
        }
    }

    for (i = i_start; i < i_stop; i+=CHUNK_STEP1) {
      __transaction_atomic {
        {
          long ii;
          long ii_stop = MIN(i_stop, (i+CHUNK_STEP1));
          for (ii = i; ii < ii_stop; ii++) {
            void* segment = vector_at(segmentsContentsPtr, ii);
            TMHASHTABLE_INSERT(uniqueSegmentsPtr, segment, segment);
          } /* ii */
        }
      }
    }

    thread_barrier_wait();

    /*
     * Step 2a: Iterate over unique segments and compute hashes.
     *
     * For the gene "atcg", the hashes for the end would be:
     *
     *     "t", "tc", and "tcg"
     *
     * And for the gene "tcgg", the hashes for the start would be:
     *
     *    "t", "tc", and "tcg"
     *
     * The names are "end" and "start" because if a matching pair is found,
     * they are the substring of the end part of the pair and the start
     * part of the pair respectively. In the above example, "tcg" is the
     * matching substring so:
     *
     *     (end)    (start)
     *     a[tcg] + [tcg]g  = a[tcg]g    (overlap = "tcg")
     */

    /* uniqueSegmentsPtr is constant now */
    numUniqueSegment = TMhashtable_getSize(uniqueSegmentsPtr);
    entryIndex = 0;

    {
        /* Choose disjoint segments [i_start,i_stop) for each thread */
        long num = uniqueSegmentsPtr->numBucket;
        long partitionSize = (num + numThread/2) / numThread; /* with rounding */
        i_start = threadId * partitionSize;
        if (threadId == (numThread - 1)) {
            i_stop = num;
        } else {
            i_stop = i_start + partitionSize;
        }
    }
    {
        /* Approximate disjoint segments of element allocation in constructEntries */
        long partitionSize = (numUniqueSegment + numThread/2) / numThread; /* with rounding */
        entryIndex = threadId * partitionSize;
    }

    for (i = i_start; i < i_stop; i++) {

        list_t* chainPtr = uniqueSegmentsPtr->buckets[i];
        list_iter_t it;
        list_iter_reset(&it, chainPtr);

        while (list_iter_hasNext(&it)) {

            char* segment =
                (char*)((pair_t*)list_iter_next(&it))->firstPtr;
            constructEntry_t* constructEntryPtr;
            long j;
            unsigned long startHash;
            bool status;

            /* Find an empty constructEntries entry */
            __transaction_atomic {
              while (((void*)constructEntries[entryIndex].segment) != NULL) {
                entryIndex = (entryIndex + 1) % numUniqueSegment; /* look for empty */
              }
              constructEntryPtr = &constructEntries[entryIndex];
              constructEntryPtr->segment = segment;
            }
            entryIndex = (entryIndex + 1) % numUniqueSegment;

            /*
             * Save hashes (sdbm algorithm) of segment substrings
             *
             * endHashes will be computed for shorter substrings after matches
             * have been made (in the next phase of the code). This will reduce
             * the number of substrings for which hashes need to be computed.
             *
             * Since we can compute startHashes incrementally, we go ahead
             * and compute all of them here.
             */
            /* constructEntryPtr is local now */
            constructEntryPtr->endHash = (unsigned long)hashString(&segment[1]);

            startHash = 0;
            for (j = 1; j < segmentLength; j++) {
                startHash = (unsigned long)segment[j-1] +
                            (startHash << 6) + (startHash << 16) - startHash;
                __transaction_atomic {
                    status = startHashToConstructEntryTables[j]->
                        insert((unsigned long)startHash, constructEntryPtr);
                }
                assert(status);
            }

            /*
             * For looking up construct entries quickly
             */
            startHash = (unsigned long)segment[j-1] +
                        (startHash << 6) + (startHash << 16) - startHash;
            __transaction_atomic {
                status = hashToConstructEntryTable->insert((unsigned long)startHash,
                                                           constructEntryPtr);
            }
            assert(status);
        }
    }

    thread_barrier_wait();

    /*
     * Step 2b: Match ends to starts by using hash-based string comparison.
     */
    for (substringLength = segmentLength-1; substringLength > 0; substringLength--) {

        table_t* startHashToConstructEntryTablePtr =
            startHashToConstructEntryTables[substringLength];
        std::set<constructEntry_t*>** buckets = startHashToConstructEntryTablePtr->buckets;
        long numBucket = startHashToConstructEntryTablePtr->numBucket;

        long index_start;
        long index_stop;

        {
            /* Choose disjoint segments [index_start,index_stop) for each thread */
            long partitionSize = (numUniqueSegment + numThread/2) / numThread; /* with rounding */
            index_start = threadId * partitionSize;
            if (threadId == (numThread - 1)) {
                index_stop = numUniqueSegment;
            } else {
                index_stop = index_start + partitionSize;
            }
        }

        /* Iterating over disjoint itervals in the range [0, numUniqueSegment) */
        for (entryIndex = index_start;
             entryIndex < index_stop;
             entryIndex += endInfoEntries[entryIndex].jumpToNext)
        {
            if (!endInfoEntries[entryIndex].isEnd) {
                continue;
            }

            /*  ConstructEntries[entryIndex] is local data */
            constructEntry_t* endConstructEntryPtr =
                &constructEntries[entryIndex];
            char* endSegment = endConstructEntryPtr->segment;
            unsigned long endHash = endConstructEntryPtr->endHash;

            std::set<constructEntry_t*>* chainPtr = buckets[endHash % numBucket]; /* buckets: constant data */

            /* Linked list at chainPtr is constant */
            for (auto i : *chainPtr) {
                constructEntry_t* startConstructEntryPtr = i;
                char* startSegment = startConstructEntryPtr->segment;
                long newLength = 0;

                /* endConstructEntryPtr is local except for properties startPtr/endPtr/length */
                __transaction_atomic {
                  /* Check if matches */
                  if (startConstructEntryPtr->isStart &&
                      (endConstructEntryPtr->startPtr != startConstructEntryPtr) &&
                      (strncmp(startSegment,
                               &endSegment[segmentLength - substringLength],
                               substringLength) == 0))
                  {
                      startConstructEntryPtr->isStart = false;

                    constructEntry_t* startConstructEntry_endPtr;
                    constructEntry_t* endConstructEntry_startPtr;

                    /* Update endInfo (appended something so no longer end) */
                    endInfoEntries[entryIndex].isEnd = false;

                    /* Update segment chain construct info */
                    startConstructEntry_endPtr = startConstructEntryPtr->endPtr;
                    endConstructEntry_startPtr = endConstructEntryPtr->startPtr;

                    assert(startConstructEntry_endPtr);
                    assert(endConstructEntry_startPtr);
                    startConstructEntry_endPtr->startPtr = endConstructEntry_startPtr;
                    endConstructEntryPtr->nextPtr = startConstructEntryPtr;
                    endConstructEntry_startPtr->endPtr = startConstructEntry_endPtr;
                    endConstructEntryPtr->overlap = substringLength;

                    newLength = endConstructEntry_startPtr->length
                        + startConstructEntryPtr->length
                        - substringLength;
                    endConstructEntry_startPtr->length = newLength;
                  } /* if (matched) */

                } // TM_END

                /* if there was a match */
                if (!endInfoEntries[entryIndex].isEnd)
                  break;

            } /* iterate over chain */

        } /* for (endIndex < numUniqueSegment) */

        thread_barrier_wait();

        /*
         * Step 2c: Update jump values and hashes
         *
         * endHash entries of all remaining ends are updated to the next
         * substringLength. Additionally jumpToNext entries are updated such
         * that they allow to skip non-end entries. Currently this is sequential
         * because parallelization did not perform better.
.        */

        if (threadId == 0) {
            if (substringLength > 1) {
                long index = segmentLength - substringLength + 1;
                /* initialization if j and i: with i being the next end after j=0 */
                for (i = 1; !endInfoEntries[i].isEnd; i+=endInfoEntries[i].jumpToNext) {
                    /* find first non-null */
                }
                /* entry 0 is handled seperately from the loop below */
                endInfoEntries[0].jumpToNext = i;
                if (endInfoEntries[0].isEnd) {
                    constructEntry_t* constructEntryPtr = &constructEntries[0];
                    char* segment = constructEntryPtr->segment;
                    constructEntryPtr->endHash = (unsigned long)hashString(&segment[index]);
                }
                /* Continue scanning (do not reset i) */
                for (j = 0; i < numUniqueSegment; i+=endInfoEntries[i].jumpToNext) {
                    if (endInfoEntries[i].isEnd) {
                        constructEntry_t* constructEntryPtr = &constructEntries[i];
                        char* segment = constructEntryPtr->segment;
                        constructEntryPtr->endHash = (unsigned long)hashString(&segment[index]);
                        endInfoEntries[j].jumpToNext = MAX(1, (i - j));
                        j = i;
                    }
                }
                endInfoEntries[j].jumpToNext = i - j;
            }
        }

        thread_barrier_wait();

    } /* for (substringLength > 0) */


    thread_barrier_wait();

    /*
     * Step 3: Build sequence string
     */
    if (threadId == 0) {

        long totalLength = 0;

        for (i = 0; i < numUniqueSegment; i++) {
            constructEntry_t* constructEntryPtr = &constructEntries[i];
            if (constructEntryPtr->isStart) {
              totalLength += constructEntryPtr->length;
            }
        }

        sequencerPtr->sequence = (char*)malloc((totalLength+1) * sizeof(char));
        char* sequence = sequencerPtr->sequence;
        assert(sequence);

        char* copyPtr = sequence;
        long sequenceLength = 0;

        for (i = 0; i < numUniqueSegment; i++) {
            constructEntry_t* constructEntryPtr = &constructEntries[i];
            /* If there are several start segments, we append in arbitrary order  */
            if (constructEntryPtr->isStart) {
                long newSequenceLength = sequenceLength + constructEntryPtr->length;
                assert( newSequenceLength <= totalLength );
                copyPtr = sequence + sequenceLength;
                sequenceLength = newSequenceLength;
                do {
                    long numChar = segmentLength - constructEntryPtr->overlap;
                    if ((copyPtr + numChar) > (sequence + newSequenceLength)) {
                        printf("ERROR: sequence length != actual length\n");
                        break;
                    }
                    memcpy(copyPtr,
                           constructEntryPtr->segment,
                           (numChar * sizeof(char)));
                    copyPtr += numChar;
                } while ((constructEntryPtr = constructEntryPtr->nextPtr) != NULL);
                assert(copyPtr <= (sequence + sequenceLength));
            }
        }

        assert(sequence != NULL);
        sequence[sequenceLength] = '\0';
    }

}


/* =============================================================================
 * sequencer_free
 * =============================================================================
 */
void
sequencer_free (sequencer_t* sequencerPtr)
{
    long i;

    delete (sequencerPtr->hashToConstructEntryTable);
    free(sequencerPtr->constructEntries);
    for (i = 1; i < sequencerPtr->segmentLength; i++) {
        delete (sequencerPtr->startHashToConstructEntryTables[i]);
    }
    free(sequencerPtr->startHashToConstructEntryTables);
    free(sequencerPtr->endInfoEntries);
    /* TODO: fix mixed sequential/parallel allocation */
    TMhashtable_free(sequencerPtr->uniqueSegmentsPtr);
    if (sequencerPtr->sequence != NULL) {
        free(sequencerPtr->sequence);
    }
    free(sequencerPtr);
}


/* =============================================================================
 * TEST_SEQUENCER
 * =============================================================================
 */
#ifdef TEST_SEQUENCER


#include <assert.h>
#include <stdio.h>
#include "segments.h"


char* gene1 = "gatcggcagc";
char* segments1[] = {
    "atcg",
    "gcag",
    "tcgg",
    "cagc",
    "gatc",
    NULL
};

char* gene2 = "aaagc";
char* segments2[] = {
    "aaa",
    "aag",
    "agc",
    NULL
};

char* gene3 = "aaacaaagaaat";
char* segments3[] = {
    "aaac",
    "aaag",
    "aaat",
    NULL
};

char* gene4 = "ttggctacgtatcgcacggt";
char* segments4[] = {
    "cgtatcgc",
    "tcgcacgg",
    "gtatcgca",
    "tatcgcac",
    "atcgcacg",
    "ttggctac",
    "ctacgtat",
    "acgtatcg",
    "ctacgtat",
    "cgtatcgc",
    "atcgcacg",
    "ggctacgt",
    "tacgtatc",
    "tcgcacgg",
    "ttggctac",
    "ggctacgt",
    "atcgcacg",
    "tatcgcac",
    "cgtatcgc",
    "acgtatcg",
    "gtatcgca",
    "gtatcgca",
    "cgcacggt",
    "tatcgcac",
    "ttggctac",
    "atcgcacg",
    "acgtatcg",
    "gtatcgca",
    "ttggctac",
    "tggctacg",
    NULL
};

char* gene5 = "gatcggcagctggtacggcg";
char* segments5[] = {
    "atcggcag",
    "gtacggcg",
    "gatcggca",
    "cagctggt",
    "tggtacgg",
    "gatcggca",
    "gatcggca",
    "tcggcagc",
    "ggtacggc",
    "tggtacgg",
    "tcggcagc",
    "gcagctgg",
    "gatcggca",
    "gctggtac",
    "gatcggca",
    "ctggtacg",
    "ggcagctg",
    "tcggcagc",
    "gtacggcg",
    "gcagctgg",
    "ggcagctg",
    "tcggcagc",
    "cagctggt",
    "tggtacgg",
    "cagctggt",
    "gcagctgg",
    "gctggtac",
    "cggcagct",
    "agctggta",
    "ctggtacg",
    NULL
};

char* gene6 = "ttggtgagccgtaagactcc";
char* segments6[] = {
    "cgtaagac",
    "taagactc",
    "gtgagccg",
    "gagccgta",
    "gccgtaag",
    "tgagccgt",
    "gccgtaag",
    "cgtaagac",
    "ttggtgag",
    "agccgtaa",
    "gccgtaag",
    "aagactcc",
    "ggtgagcc",
    "ttggtgag",
    "agccgtaa",
    "gagccgta",
    "aagactcc",
    "ttggtgag",
    "gtaagact",
    "ccgtaaga",
    "ttggtgag",
    "gagccgta",
    "ggtgagcc",
    "gagccgta",
    "gccgtaag",
    "aagactcc",
    "gtaagact",
    "ccgtaaga",
    "tgagccgt",
    "ttggtgag",
    NULL
};

char* gene7 = "gatcggcagctggtacggcg";
char* segments7[] = {
    "atcggcag",
    "gtacggcg",
    "gatcggca",
    "cagctggt",
    "tggtacgg",
    "gatcggca",
    "gatcggca",
    "tcggcagc",
    "ggtacggc",
    "tggtacgg",
    "tcggcagc",
    "gcagctgg",
    "gatcggca",
    "gctggtac",
    "gatcggca",
    "ctggtacg",
    "ggcagctg",
    "tcggcagc",
    "gtacggcg",
    "gcagctgg",
    "ggcagctg",
    "tcggcagc",
    "cagctggt",
    "tggtacgg",
    "cagctggt",
    "gcagctgg",
    "gctggtac",
    "cggcagct",
    "agctggta",
    "ctggtacg",
    NULL
};

char* gene8 = "ttggtgagccgtaagactcc";
char* segments8[] = {
    "cgtaagac",
    "taagactc",
    "gtgagccg",
    "gagccgta",
    "gccgtaag",
    "tgagccgt",
    "gccgtaag",
    "cgtaagac",
    "ttggtgag",
    "agccgtaa",
    "gccgtaag",
    "aagactcc",
    "ggtgagcc",
    "ttggtgag",
    "agccgtaa",
    "gagccgta",
    "aagactcc",
    "ttggtgag",
    "gtaagact",
    "ccgtaaga",
    "ttggtgag",
    "gagccgta",
    "ggtgagcc",
    "gagccgta",
    "gccgtaag",
    "aagactcc",
    "gtaagact",
    "ccgtaaga",
    "tgagccgt",
    "ttggtgag",
    NULL
};


static segments_t*
createSegments (char* segments[])
{
    long i = 0;
    segments_t* segmentsPtr = (segments_t*)malloc(sizeof(segments));

    segmentsPtr->length = strlen(segments[0]);
    segmentsPtr->contentsPtr = vector_alloc(1);

    while (segments[i] != NULL) {
        bool status = vector_pushBack(segmentsPtr->contentsPtr,
                                        (void*)segments[i]);
        assert(status);
        i++;
    }

    segmentsPtr->minNum = vector_getSize(segmentsPtr->contentsPtr);

    return segmentsPtr;
}


static void
tester (char* gene, char* segments[])
{
    segments_t* segmentsPtr;
    sequencer_t* sequencerPtr;

    segmentsPtr = createSegments(segments);
    sequencerPtr = sequencer_alloc(strlen(gene), segmentsPtr->length, segmentsPtr);

    sequencer_run((void*)sequencerPtr);

    printf("gene     = %s\n", gene);
    printf("sequence = %s\n", sequencerPtr->sequence);
    assert(strcmp(sequencerPtr->sequence, gene) == 0);

    sequencer_free(sequencerPtr);
}


int
main ()
{
    bool status = memory_init(1, 4, 2);
    assert(status);
    thread_startup(1);

    puts("Starting...");

    /* Simple test */
    tester(gene1, segments1);

    /* Simple test with aliasing segments */
    tester(gene2, segments2);

    /* Simple test with non-overlapping segments */
    tester(gene3, segments3);

    /* Complex tests */
    tester(gene4, segments4);
    tester(gene5, segments5);
    tester(gene6, segments6);
    tester(gene7, segments7);
    tester(gene8, segments8);

    puts("Passed all tests.");

    return 0;
}


#endif /* TEST_SEQUENCER */
