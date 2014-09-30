/*
 * PLEASE SEE LICENSE FILE FOR LICENSING AND COPYRIGHT INFORMATION
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "element.h"
#include "mesh.h"
#include "utility.h"
#include "tm_transition.h"

mesh_t::mesh_t()
{
    rootElementPtr = NULL;
    initBadQueuePtr = queue_alloc(-1);
    assert(initBadQueuePtr);
    size = 0;
    boundarySetPtr = SET_ALLOC(NULL, &element_listCompareEdge);
    assert(boundarySetPtr);
}

mesh_t::~mesh_t()
{
    queue_free(initBadQueuePtr);
    SET_FREE(boundarySetPtr);
}

__attribute__((transaction_safe))
void mesh_t::insert(element_t* elementPtr, MAP_T* edgeMapPtr)
{
    /*
     * Assuming fully connected graph, we just need to record one element.
     * The root element is not needed for the actual refining, but rather
     * for checking the validity of the final mesh.
     */
    if (!rootElementPtr) {
        rootElementPtr = elementPtr;
    }

    /*
     * Record existence of each of this element's edges
     */
    long i;
    long numEdge = elementPtr->getNumEdge();
    for (i = 0; i < numEdge; i++) {
        edge_t* edgePtr = elementPtr->getEdge(i);
        if (!MAP_CONTAINS(edgeMapPtr, (void*)edgePtr)) {
            /* Record existance of this edge */
            bool isSuccess;
            isSuccess =
                MAP_INSERT(edgeMapPtr, (void*)edgePtr, (void*)elementPtr);
            assert(isSuccess);
        } else {
            /*
             * Shared edge; update each element's neighborList
             */
            bool isSuccess;
            element_t* sharerPtr = (element_t*)MAP_FIND(edgeMapPtr, edgePtr);
            assert(sharerPtr); /* cannot be shared by >2 elements */
            elementPtr->addNeighbor(sharerPtr);
            sharerPtr->addNeighbor(elementPtr);
            isSuccess = MAP_REMOVE(edgeMapPtr, edgePtr);
            assert(isSuccess);
            isSuccess = MAP_INSERT(edgeMapPtr, edgePtr, NULL); /* marker to check >2 sharers */
            assert(isSuccess);
        }
    }

    /*
     * Check if really encroached
     */

    edge_t* encroachedPtr = elementPtr->getEncroachedPtr();
    if (encroachedPtr) {
        if (!TMSET_CONTAINS(boundarySetPtr, encroachedPtr)) {
            elementPtr->clearEncroached();
        }
    }
}

__attribute__((transaction_safe))
void mesh_t::remove(element_t* elementPtr)
{
    assert(!elementPtr->isEltGarbage());

    /*
     * If removing root, a new root is selected on the next mesh_insert, which
     * always follows a call a mesh_remove.
     */
    if (rootElementPtr == elementPtr) {
        rootElementPtr = NULL;
    }

    /*
     * Remove from neighbors
     */
    list_iter_t it;
    //list_t* neighborListPtr = element_getNeighborListPtr(elementPtr);
    list_t* neighborListPtr = elementPtr->neighborListPtr;
    //TMLIST_ITER_RESET(&it, neighborListPtr);
    it = &(neighborListPtr->head);

    while (it->nextPtr != NULL) {
    //while (TMLIST_ITER_HASNEXT(&it)) {
      //element_t* neighborPtr =
      //      (element_t*)TMLIST_ITER_NEXT(&it, neighborListPtr);
      it = it->nextPtr;
      element_t * neighborPtr = (element_t*)it->dataPtr;

      //list_t* neighborNeighborListPtr = element_getNeighborListPtr(neighborPtr);
      list_t* neighborNeighborListPtr = neighborPtr->neighborListPtr;
        bool status = TMLIST_REMOVE(neighborNeighborListPtr, elementPtr);
        assert(status);
    }

    //TMELEMENT_SETISGARBAGE(elementPtr, true);
    elementPtr->isGarbage = true;

    //if (!TMELEMENT_ISREFERENCED(elementPtr)) {
    if (!elementPtr->isReferenced) {
        delete elementPtr;
    }
}

__attribute__((transaction_safe))
bool mesh_t::insertBoundary(edge_t* boundaryPtr)
{
    return TMSET_INSERT(boundarySetPtr, boundaryPtr);
}

__attribute__((transaction_safe))
bool mesh_t::removeBoundary(edge_t* boundaryPtr)
{
    return TMSET_REMOVE(boundarySetPtr, boundaryPtr);
}

/* =============================================================================
 * createElement
 * =============================================================================
 */
static void
createElement (mesh_t* meshPtr,
               coordinate_t* coordinates,
               long numCoordinate,
               MAP_T* edgeMapPtr)
{
    element_t* elementPtr = new element_t(coordinates, numCoordinate);
    assert(elementPtr);

    if (numCoordinate == 2) {
        edge_t* boundaryPtr = elementPtr->getEdge(0);
        bool status = SET_INSERT(meshPtr->boundarySetPtr, boundaryPtr);
        assert(status);
    }

    meshPtr->insert(elementPtr, edgeMapPtr);

    if (elementPtr->isBad()) {
        bool status = queue_push(meshPtr->initBadQueuePtr, (void*)elementPtr);
        assert(status);
    }
 }


long mesh_t::read(const char* fileNamePrefix)
{
    FILE* inputFile;
    coordinate_t* coordinates;
    char fileName[256];
    long fileNameSize = sizeof(fileName) / sizeof(fileName[0]);
    char inputBuff[256];
    long inputBuffSize = sizeof(inputBuff) / sizeof(inputBuff[0]);
    long numEntry;
    long numDimension;
    long numCoordinate;
    long i;
    long numElement = 0;

    MAP_T* edgeMapPtr = MAP_ALLOC(NULL, &element_mapCompareEdge);
    assert(edgeMapPtr);

    /*
     * Read .node file
     */
    snprintf(fileName, fileNameSize, "%s.node", fileNamePrefix);
    inputFile = fopen(fileName, "r");
    assert(inputFile);
    fgets(inputBuff, inputBuffSize, inputFile);
    sscanf(inputBuff, "%li %li", &numEntry, &numDimension);
    assert(numDimension == 2); /* must be 2-D */
    numCoordinate = numEntry + 1; /* numbering can start from 1 */
    coordinates = (coordinate_t*)malloc(numCoordinate * sizeof(coordinate_t));
    assert(coordinates);
    for (i = 0; i < numEntry; i++) {
        long id;
        double x;
        double y;
        if (!fgets(inputBuff, inputBuffSize, inputFile)) {
            break;
        }
        if (inputBuff[0] == '#') {
            continue; /* TODO: handle comments correctly */
        }
        sscanf(inputBuff, "%li %lf %lf", &id, &x, &y);
        coordinates[id].x = x;
        coordinates[id].y = y;
    }
    assert(i == numEntry);
    fclose(inputFile);

    /*
     * Read .poly file, which contains boundary segments
     */
    snprintf(fileName, fileNameSize, "%s.poly", fileNamePrefix);
    inputFile = fopen(fileName, "r");
    assert(inputFile);
    fgets(inputBuff, inputBuffSize, inputFile);
    sscanf(inputBuff, "%li %li", &numEntry, &numDimension);
    assert(numEntry == 0); /* .node file used for vertices */
    assert(numDimension == 2); /* must be edge */
    fgets(inputBuff, inputBuffSize, inputFile);
    sscanf(inputBuff, "%li", &numEntry);
    for (i = 0; i < numEntry; i++) {
        long id;
        long a;
        long b;
        coordinate_t insertCoordinates[2];
        if (!fgets(inputBuff, inputBuffSize, inputFile)) {
            break;
        }
        if (inputBuff[0] == '#') {
            continue; /* TODO: handle comments correctly */
        }
        sscanf(inputBuff, "%li %li %li", &id, &a, &b);
        assert(a >= 0 && a < numCoordinate);
        assert(b >= 0 && b < numCoordinate);
        insertCoordinates[0] = coordinates[a];
        insertCoordinates[1] = coordinates[b];
        createElement(this, insertCoordinates, 2, edgeMapPtr);
    }
    assert(i == numEntry);
    numElement += numEntry;
    fclose(inputFile);

    /*
     * Read .ele file, which contains triangles
     */
    snprintf(fileName, fileNameSize, "%s.ele", fileNamePrefix);
    inputFile = fopen(fileName, "r");
    assert(inputFile);
    fgets(inputBuff, inputBuffSize, inputFile);
    sscanf(inputBuff, "%li %li", &numEntry, &numDimension);
    assert(numDimension == 3); /* must be triangle */
    for (i = 0; i < numEntry; i++) {
        long id;
        long a;
        long b;
        long c;
        coordinate_t insertCoordinates[3];
        if (!fgets(inputBuff, inputBuffSize, inputFile)) {
            break;
        }
        if (inputBuff[0] == '#') {
            continue; /* TODO: handle comments correctly */
        }
        sscanf(inputBuff, "%li %li %li %li", &id, &a, &b, &c);
        assert(a >= 0 && a < numCoordinate);
        assert(b >= 0 && b < numCoordinate);
        assert(c >= 0 && c < numCoordinate);
        insertCoordinates[0] = coordinates[a];
        insertCoordinates[1] = coordinates[b];
        insertCoordinates[2] = coordinates[c];
        createElement(this, insertCoordinates, 3, edgeMapPtr);
    }
    assert(i == numEntry);
    numElement += numEntry;
    fclose(inputFile);

    free(coordinates);
    MAP_FREE(edgeMapPtr);

    return numElement;
}


/* =============================================================================
 * mesh_getBad
 * -- Returns NULL if none
 * =============================================================================
 */
element_t* mesh_t::getBad()
{
    return (element_t*)queue_pop(initBadQueuePtr);
}

void mesh_t::shuffleBad(std::mt19937* randomPtr)
{
    queue_shuffle(initBadQueuePtr, randomPtr);
}


/* =============================================================================
 * mesh_check
 * =============================================================================
 */
bool mesh_t::check(long expectedNumElement)
{
    queue_t* searchQueuePtr;
    MAP_T* visitedMapPtr;
    long numBadTriangle = 0;
    long numFalseNeighbor = 0;
    long numElement = 0;

    puts("Checking final mesh:");
    fflush(stdout);

    searchQueuePtr = queue_alloc(-1);
    assert(searchQueuePtr);
    visitedMapPtr = MAP_ALLOC(NULL, &element_mapCompare);
    assert(visitedMapPtr);

    /*
     * Do breadth-first search starting from rootElementPtr
     */
    assert(rootElementPtr);
    queue_push(searchQueuePtr, (void*)rootElementPtr);
    while (!queue_isEmpty(searchQueuePtr)) {

        element_t* currentElementPtr;
        list_iter_t it;
        list_t* neighborListPtr;
        bool isSuccess;

        currentElementPtr = (element_t*)queue_pop(searchQueuePtr);
        if (MAP_CONTAINS(visitedMapPtr, (void*)currentElementPtr)) {
            continue;
        }
        isSuccess = MAP_INSERT(visitedMapPtr, (void*)currentElementPtr, NULL);
        assert(isSuccess);
        if (!element_checkAngles(currentElementPtr)) {
            numBadTriangle++;
        }
        neighborListPtr = currentElementPtr->getNeighborListPtr();

        list_iter_reset(&it, neighborListPtr);
        while (list_iter_hasNext(&it)) {
            element_t* neighborElementPtr =
                (element_t*)list_iter_next(&it);
            /*
             * Continue breadth-first search
             */
            if (!MAP_CONTAINS(visitedMapPtr, (void*)neighborElementPtr)) {
                bool isSuccess;
                isSuccess = queue_push(searchQueuePtr,
                                       (void*)neighborElementPtr);
                assert(isSuccess);
            }
        } /* for each neighbor */

        numElement++;

    } /* breadth-first search */

    printf("Number of elements      = %li\n", numElement);
    printf("Number of bad triangles = %li\n", numBadTriangle);

    queue_free(searchQueuePtr);
    MAP_FREE(visitedMapPtr);

    return ((numBadTriangle > 0 ||
             numFalseNeighbor > 0 ||
             numElement != expectedNumElement) ? false : true);
}


#ifdef TEST_MESH
/* =============================================================================
 * TEST_MESH
 * =============================================================================
 */


#include <stdio.h>


int
main (int argc, char* argv[])
{
    mesh_t* meshPtr;

    assert(argc == 2);

    puts("Starting tests...");

    meshPtr = mesh_alloc();
    assert(meshPtr);

    mesh_read(meshPtr, argv[1]);

    mesh_free(meshPtr);

    puts("All tests passed.");

    return 0;
}


#endif /* TEST_MESH */
