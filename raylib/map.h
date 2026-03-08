#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAP_LAYERS 15
#define MAP_MAX_NODES_PER_LAYER 4
#define MAP_MAX_EDGES 6
#define MAP_MAX_NODES (MAP_LAYERS * MAP_MAX_NODES_PER_LAYER)

typedef enum {
    NODE_COMBAT = 0,
    NODE_ELITE,
    NODE_BOSS,
    NODE_SHOP,
    NODE_REST,
    NODE_EVENT,
    NODE_COUNT
} NodeType;

typedef struct {
    NodeType type;
    int layer;
    int column;
    bool visited;
    bool available;
    float x, y;     // screen position (computed at render)
    int edgeCount;
    int edges[MAP_MAX_EDGES];
} MapNode;

typedef struct {
    MapNode nodes[MAP_MAX_NODES];
    int nodeCount;
    int nodesPerLayer[MAP_LAYERS];
    int layerOffset[MAP_LAYERS];
    int currentLayer;   // -1 = not started
    int currentNode;    // index of last visited node (-1 = none)
    int act;
    uint32_t seed;
} ActMap;

// Event types for random events on EVENT nodes
typedef enum {
    EVENT_FOUNTAIN = 0,
    EVENT_MERCHANT,
    EVENT_TRAINING,
    EVENT_ALTAR,
    EVENT_TOME,
    EVENT_COUNT
} MapEventType;

// Generate a new act map
void GenerateMap(ActMap *map, int act, uint32_t seed);

// Returns the NodeType of the selected node, or -1 if nothing selected.
// Sets map->currentNode and marks visited/available.
int UpdateMap(ActMap *map);

// Draw the map screen
void DrawMap(ActMap *map);

// Reset map scroll to show bottom (layer 0)
void ResetMapScroll(void);

// Get a random event type for an event node
MapEventType GetRandomEvent(uint32_t seed);
