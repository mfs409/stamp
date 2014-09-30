/*
 * PLEASE SEE LICENSE FILE FOR LICENSING AND COPYRIGHT INFORMATION
 */

#pragma once

#include "coordinate.h"
#include "list.h"
#include "pair.h"

typedef pair_t         edge_t;
struct element_t {
    coordinate_t coordinates[3];
    long numCoordinate;
    coordinate_t circumCenter;
    double circumRadius;
    double minAngle;
    edge_t edges[3];
    long numEdge;
    coordinate_t midpoints[3]; /* midpoint of each edge */
    double radii[3];           /* half of edge length */
    edge_t* encroachedEdgePtr; /* opposite obtuse angle */
    bool isSkinny;
    list_t* neighborListPtr;
    bool isGarbage;
    bool isReferenced;

  __attribute__((transaction_safe))
  element_t(coordinate_t* coordinates, long numCoordinate);

  __attribute__((transaction_safe))
  ~element_t();

  __attribute__((transaction_safe))
  long getNumEdge();

  /*
   * element_getEdge: Returned edgePtr is sorted; i.e., coordinate_compare(first, second) < 0
   */
  __attribute__((transaction_safe))
  edge_t* getEdge(long i);

  __attribute__((transaction_safe))
  bool isInCircumCircle(coordinate_t* coordinatePtr);

  __attribute__((transaction_safe))
  void clearEncroached();

  __attribute__((transaction_safe))
  edge_t* getEncroachedPtr();

  bool isEltSkinny();

  /*
   * element_isBad: Does it need to be refined?
   */
  __attribute__((transaction_safe))
  bool isBad();

  __attribute__((transaction_safe))
  void setIsReferenced(bool status);
};

__attribute__((transaction_safe))
long element_compare(element_t* aElementPtr, element_t* bElementPtr);

__attribute__((transaction_safe))
long element_listCompare(const void* aPtr, const void* bPtr);

__attribute__((transaction_safe))
long element_mapCompare(const pair_t* aPtr, const pair_t* bPtr);

__attribute__((transaction_safe))
long element_listCompareEdge(const void* aPtr, const void* bPtr);

__attribute__((transaction_safe))
long element_mapCompareEdge(const pair_t* aPtr, const pair_t* bPtr);

__attribute__((transaction_safe))
long element_heapCompare(const void* aPtr, const void* bPtr);


// these should become methods of element_t

/*
 * TMelement_isGarbage: Can we deallocate?
 */
__attribute__((transaction_safe))
bool TMelement_isGarbage(element_t* elementPtr);

__attribute__((transaction_safe))
void TMelement_setIsGarbage(element_t* elementPtr, bool status);

__attribute__((transaction_safe))
void TMelement_addNeighbor(element_t* elementPtr, element_t* neighborPtr);

__attribute__((transaction_safe))
list_t* element_getNeighborListPtr(element_t* elementPtr);

__attribute__((transaction_safe))
edge_t* element_getCommonEdge(element_t* aElementPtr, element_t* bElementPtr);

/*
 * element_getNewPoint:  Either the element is encroached or is skinny, so get the new point to add
 */
//[wer210] previous returns a struct, which causes errors
// coordinate_t element_getNewPoint(element_t* elementPtr);
__attribute__((transaction_safe))
void element_getNewPoint(element_t* elementPtr, coordinate_t* ret);

/*
 * element_checkAngles: Return false if minimum angle constraint not met
 */
bool element_checkAngles(element_t* elementPtr);

void element_print(element_t* elementPtr);

void element_printEdge(edge_t* edgePtr);

void element_printAngles(element_t* elementPtr);
