#include "map.h"
#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <string.h>

// Simple seeded RNG for map generation (independent of combat RNG)
static uint32_t mapRng;
static void map_rng_seed(uint32_t s) { mapRng = s ? s : 1; }
static uint32_t map_rng_next_u32(void) {
  mapRng ^= mapRng << 13;
  mapRng ^= mapRng >> 17;
  mapRng ^= mapRng << 5;
  return mapRng;
}
static int map_rng_range(int lo, int hi) {
  if (lo >= hi)
    return lo;
  return lo + (int)(map_rng_next_u32() % (uint32_t)(hi - lo + 1));
}
static float map_rng_float(void) {
  return (float)(map_rng_next_u32() & 0xFFFF) / 65535.0f;
}

static const char *NODE_NAMES[] = {"Combat", "Elite", "Boss",
                                   "Shop",   "Rest",  "Event"};

static Color NODE_COLORS[] = {
    {140, 140, 140, 255}, // Combat: gray
    {240, 200, 40, 255},  // Elite: yellow
    {220, 50, 50, 255},   // Boss: red
    {50, 200, 80, 255},   // Shop: green
    {80, 200, 220, 255},  // Rest: cyan
    {180, 80, 220, 255},  // Event: purple
};

// Pick node type based on weighted distribution and constraints
static NodeType PickNodeType(int layer, int prevLayerHasShop,
                             int prevLayerHasRest) {
  // Layer 0: always combat
  if (layer == 0)
    return NODE_COMBAT;
  // Last layer: always boss
  if (layer == MAP_LAYERS - 1)
    return NODE_BOSS;

  // Elites can't appear before layer 5
  // Roll weighted random
  for (int attempt = 0; attempt < 20; attempt++) {
    float r = map_rng_float();
    NodeType t;
    if (r < 0.45f)
      t = NODE_COMBAT;
    else if (r < 0.57f)
      t = NODE_ELITE;
    else if (r < 0.67f)
      t = NODE_SHOP;
    else if (r < 0.82f)
      t = NODE_REST;
    else
      t = NODE_EVENT;

    // Constraints
    if (t == NODE_ELITE && layer < 5)
      continue;
    if (t == NODE_SHOP && prevLayerHasShop)
      continue;
    if (t == NODE_REST && prevLayerHasRest)
      continue;
    return t;
  }
  return NODE_COMBAT; // fallback
}

void GenerateMap(ActMap *map, int act, uint32_t seed) {
  memset(map, 0, sizeof(ActMap));
  map->act = act;
  map->seed = seed;
  map->currentLayer = -1;
  map->currentNode = -1;

  map_rng_seed(seed + act * 1000);

  int nodeIdx = 0;

  // Generate nodes per layer
  for (int L = 0; L < MAP_LAYERS; L++) {
    int count;
    if (L == 0)
      count = 1;
    else if (L == MAP_LAYERS - 1)
      count = 1;
    else
      count = map_rng_range(2, MAP_MAX_NODES_PER_LAYER);

    map->nodesPerLayer[L] = count;
    map->layerOffset[L] = nodeIdx;

    // Determine if previous layer has shop/rest (for constraints)
    int prevShop = 0, prevRest = 0;
    if (L > 0) {
      for (int n = map->layerOffset[L - 1];
           n < map->layerOffset[L - 1] + map->nodesPerLayer[L - 1]; n++) {
        if (map->nodes[n].type == NODE_SHOP)
          prevShop = 1;
        if (map->nodes[n].type == NODE_REST)
          prevRest = 1;
      }
    }

    for (int c = 0; c < count; c++) {
      MapNode *node = &map->nodes[nodeIdx];
      node->type = PickNodeType(L, prevShop, prevRest);
      node->layer = L;
      node->column = c;
      node->visited = false;
      node->available = (L == 0); // only first layer available initially
      node->edgeCount = 0;
      nodeIdx++;
    }
  }
  map->nodeCount = nodeIdx;

  // Ensure at least one elite in layers 5-12
  bool hasElite = false;
  for (int L = 5; L <= 12 && L < MAP_LAYERS - 1; L++) {
    for (int n = map->layerOffset[L];
         n < map->layerOffset[L] + map->nodesPerLayer[L]; n++) {
      if (map->nodes[n].type == NODE_ELITE) {
        hasElite = true;
        break;
      }
    }
    if (hasElite)
      break;
  }
  if (!hasElite) {
    // Force an elite on a random node in layers 5-12
    int targetLayer =
        map_rng_range(5, 12 < MAP_LAYERS - 2 ? 12 : MAP_LAYERS - 2);
    int ni = map->layerOffset[targetLayer] +
             map_rng_range(0, map->nodesPerLayer[targetLayer] - 1);
    map->nodes[ni].type = NODE_ELITE;
  }

  // Generate edges (paths between layers)
  // StS rule: paths must not cross. If node A.col < B.col, A's edges <= B's
  // edges in column.
  for (int L = 0; L < MAP_LAYERS - 1; L++) {
    int curCount = map->nodesPerLayer[L];
    int nextCount = map->nodesPerLayer[L + 1];
    int curBase = map->layerOffset[L];
    int nextBase = map->layerOffset[L + 1];

    // Ensure every current node has at least one edge
    // and every next node is reachable
    // Start with mandatory connections: map each current node to proportional
    // next node
    int minEdge[MAP_MAX_NODES_PER_LAYER];
    int maxEdge[MAP_MAX_NODES_PER_LAYER];

    // Each current node maps to at least one proportional next node
    for (int c = 0; c < curCount; c++) {
      int primary = (nextCount > 1)
                        ? (c * (nextCount - 1) + (curCount - 1) / 2) /
                              (curCount > 1 ? curCount - 1 : 1)
                        : 0;
      if (primary >= nextCount)
        primary = nextCount - 1;
      minEdge[c] = primary;
      maxEdge[c] = primary;
    }

    // Ensure every next-layer node is reachable
    for (int n = 0; n < nextCount; n++) {
      bool reachable = false;
      for (int c = 0; c < curCount; c++) {
        if (minEdge[c] <= n && maxEdge[c] >= n) {
          reachable = true;
          break;
        }
      }
      if (!reachable) {
        // Find nearest current node and extend its range
        int bestC = 0;
        int bestDist = 9999;
        for (int c = 0; c < curCount; c++) {
          int d = (n < minEdge[c]) ? minEdge[c] - n : n - maxEdge[c];
          if (d < bestDist) {
            bestDist = d;
            bestC = c;
          }
        }
        if (n < minEdge[bestC])
          minEdge[bestC] = n;
        else
          maxEdge[bestC] = n;
      }
    }

    // Enforce no-crossing: minEdge and maxEdge must be non-decreasing
    for (int c = 1; c < curCount; c++) {
      if (minEdge[c] < minEdge[c - 1])
        minEdge[c] = minEdge[c - 1];
      if (maxEdge[c] < maxEdge[c - 1])
        maxEdge[c] = maxEdge[c - 1];
    }

    // Optionally add extra random edges (1-2 per node) within non-crossing
    // bounds
    for (int c = 0; c < curCount; c++) {
      MapNode *node = &map->nodes[curBase + c];
      // Add all nodes in [minEdge, maxEdge] range
      for (int n = minEdge[c];
           n <= maxEdge[c] && node->edgeCount < MAP_MAX_EDGES; n++) {
        node->edges[node->edgeCount++] = nextBase + n;
      }
      // Maybe extend range by 1 in either direction (if non-crossing holds)
      if (map_rng_float() < 0.3f && node->edgeCount < MAP_MAX_EDGES) {
        int extra = maxEdge[c] + 1;
        if (extra < nextCount) {
          // Check non-crossing: no later node should have a min below this
          bool ok = true;
          for (int c2 = c + 1; c2 < curCount; c2++) {
            if (minEdge[c2] < extra) {
              ok = false;
              break;
            }
          }
          if (ok)
            node->edges[node->edgeCount++] = nextBase + extra;
        }
      }
    }
  }
}

// Scroll state
static float mapScrollY = 0.0f;
static float mapScrollTarget = 0.0f;

void ResetMapScroll(void) {
  // Show layer 0 (bottom of map) on first open
  ScrollMapToLayer(0);
}

void ScrollMapToLayer(int layer) {
  // In DrawMap, layer L is drawn at: baseY - scrollY +
  // (MAP_LAYERS-1-L)*layerSpacing We want that Y position at roughly 60% down
  // the screen (so you see layers above).
  float baseY = 60.0f;
  float layerSpacing = 80.0f;
  float screenH = (float)GetScreenHeight();
  float layerDrawPos = (float)(MAP_LAYERS - 1 - layer) * layerSpacing;
  // desiredScroll: baseY - scroll + layerDrawPos = screenH * 0.6
  // scroll = baseY + layerDrawPos - screenH * 0.6
  float desiredScroll = baseY + layerDrawPos - screenH * 0.6f;
  if (desiredScroll < 0)
    desiredScroll = 0;
  float maxScroll = (MAP_LAYERS - 1) * layerSpacing;
  if (desiredScroll > maxScroll)
    desiredScroll = maxScroll;
  mapScrollY = desiredScroll;
  mapScrollTarget = desiredScroll;
}

int UpdateMap(ActMap *map) {
  // Smooth scroll
  float scrollSpeed = 12.0f;
  mapScrollY += (mapScrollTarget - mapScrollY) * GetFrameTime() * scrollSpeed;

  // Mouse wheel scroll
  float wheel = GetMouseWheelMove();
  if (wheel != 0) {
    mapScrollTarget -= wheel * 60.0f;
    if (mapScrollTarget < 0)
      mapScrollTarget = 0;
    float maxScroll = (MAP_LAYERS - 1) * 80.0f;
    if (mapScrollTarget > maxScroll)
      mapScrollTarget = maxScroll;
  }

  if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    return -1;

  Vector2 mouse = GetMousePosition();

  for (int i = 0; i < map->nodeCount; i++) {
    if (!map->nodes[i].available)
      continue;
    float nx = map->nodes[i].x;
    float ny = map->nodes[i].y;
    float radius = 18.0f;
    if (CheckCollisionPointCircle(mouse, (Vector2){nx, ny}, radius)) {
      // Select this node
      map->nodes[i].visited = true;
      map->nodes[i].available = false;
      map->currentNode = i;
      map->currentLayer = map->nodes[i].layer;

      // Clear all available flags
      for (int j = 0; j < map->nodeCount; j++)
        map->nodes[j].available = false;

      // Set available on connected nodes in next layer
      for (int e = 0; e < map->nodes[i].edgeCount; e++) {
        int next = map->nodes[i].edges[e];
        if (next >= 0 && next < map->nodeCount)
          map->nodes[next].available = true;
      }

      // Auto-scroll to show available nodes
      if (map->nodes[i].layer < MAP_LAYERS - 1) {
        float targetLayerY = (float)(map->nodes[i].layer + 1) * 80.0f;
        float screenH = (float)GetScreenHeight();
        float desiredScroll = targetLayerY - screenH * 0.6f;
        if (desiredScroll < 0)
          desiredScroll = 0;
        mapScrollTarget = desiredScroll;
      }

      return (int)map->nodes[i].type;
    }
  }
  return -1;
}

void DrawMap(ActMap *map) {
  int sw = GetScreenWidth();
  int sh = GetScreenHeight();

  // Dark background
  DrawRectangle(0, 0, sw, sh, (Color){15, 15, 25, 255});

  // Title
  const char *title = TextFormat("ACT %d", map->act);
  int tw = MeasureText(title, 30);
  DrawText(title, sw / 2 - tw / 2, 15, 30, (Color){200, 180, 120, 255});

  float baseY = 60.0f; // top padding
  float layerSpacing = 80.0f;
  float offsetY = baseY - mapScrollY;

  // Compute node positions
  for (int L = 0; L < MAP_LAYERS; L++) {
    int count = map->nodesPerLayer[L];
    float layerY = offsetY + (float)(MAP_LAYERS - 1 - L) *
                                 layerSpacing; // bottom=layer 0, top=boss
    float totalWidth = (float)(count - 1) * 100.0f;
    float startX = (float)sw / 2.0f - totalWidth / 2.0f;

    for (int c = 0; c < count; c++) {
      int ni = map->layerOffset[L] + c;
      map->nodes[ni].x = startX + (float)c * 100.0f;
      map->nodes[ni].y = layerY;
    }
  }

  // Draw edges
  for (int i = 0; i < map->nodeCount; i++) {
    MapNode *node = &map->nodes[i];
    for (int e = 0; e < node->edgeCount; e++) {
      int next = node->edges[e];
      if (next < 0 || next >= map->nodeCount)
        continue;
      MapNode *nextNode = &map->nodes[next];

      Color lineColor = (Color){50, 50, 70, 255}; // dark
      if (node->visited && nextNode->visited)
        lineColor = (Color){180, 160, 100, 255}; // visited path
      else if (node->visited && nextNode->available)
        lineColor = (Color){120, 120, 160, 255}; // available path

      // Draw curved line (bezier-ish)
      float midY = (node->y + nextNode->y) / 2.0f;
      Vector2 p0 = {node->x, node->y};
      Vector2 p1 = {node->x, midY};
      Vector2 p2 = {nextNode->x, midY};
      Vector2 p3 = {nextNode->x, nextNode->y};

      // Simple line segments approximating a curve
      Vector2 prev = p0;
      for (int t = 1; t <= 8; t++) {
        float f = (float)t / 8.0f;
        float u = 1.0f - f;
        float x = u * u * u * p0.x + 3 * u * u * f * p1.x +
                  3 * u * f * f * p2.x + f * f * f * p3.x;
        float y = u * u * u * p0.y + 3 * u * u * f * p1.y +
                  3 * u * f * f * p2.y + f * f * f * p3.y;
        DrawLineEx(prev, (Vector2){x, y}, 2.0f, lineColor);
        prev = (Vector2){x, y};
      }
    }
  }

  // Draw nodes
  Vector2 mouse = GetMousePosition();
  int hoveredNode = -1;

  for (int i = 0; i < map->nodeCount; i++) {
    MapNode *node = &map->nodes[i];
    float nx = node->x;
    float ny = node->y;
    float radius = 16.0f;

    // Skip offscreen nodes
    if (ny < -30 || ny > sh + 30)
      continue;

    Color col = NODE_COLORS[node->type];
    Color bg = col;

    if (node->visited) {
      // Dimmed
      bg.r /= 3;
      bg.g /= 3;
      bg.b /= 3;
      bg.a = 180;
    } else if (node->available) {
      // Glow pulse
      float pulse = 0.7f + 0.3f * sinf((float)GetTime() * 4.0f);
      bg.r = (unsigned char)((float)bg.r * pulse);
      bg.g = (unsigned char)((float)bg.g * pulse);
      bg.b = (unsigned char)((float)bg.b * pulse);
      radius = 18.0f;
    } else {
      // Dark / locked
      bg.r /= 2;
      bg.g /= 2;
      bg.b /= 2;
      bg.a = 120;
    }

    DrawCircleV((Vector2){nx, ny}, radius, bg);
    DrawCircleLinesV((Vector2){nx, ny}, radius, col);

    // Draw icon/letter in node
    const char *label;
    switch (node->type) {
    case NODE_COMBAT:
      label = "C";
      break;
    case NODE_ELITE:
      label = "E";
      break;
    case NODE_BOSS:
      label = "B";
      break;
    case NODE_SHOP:
      label = "$";
      break;
    case NODE_REST:
      label = "R";
      break;
    case NODE_EVENT:
      label = "?";
      break;
    default:
      label = "?";
      break;
    }
    int lw = MeasureText(label, 16);
    DrawText(label, (int)(nx - lw / 2), (int)(ny - 8), 16, WHITE);

    // Current position marker
    if (i == map->currentNode) {
      DrawCircleLinesV((Vector2){nx, ny}, radius + 4, GOLD);
      DrawCircleLinesV((Vector2){nx, ny}, radius + 5, GOLD);
    }

    // Hover detection
    if (CheckCollisionPointCircle(mouse, (Vector2){nx, ny}, radius))
      hoveredNode = i;
  }

  // Layer labels on left
  for (int L = 0; L < MAP_LAYERS; L++) {
    if (map->nodesPerLayer[L] == 0)
      continue;
    float ly = map->nodes[map->layerOffset[L]].y;
    if (ly < -20 || ly > sh + 20)
      continue;
    const char *floorLabel = TextFormat("%d", L + 1);
    DrawText(floorLabel, 10, (int)(ly - 8), 14, (Color){80, 80, 100, 200});
  }

  // Tooltip on hover
  if (hoveredNode >= 0) {
    MapNode *node = &map->nodes[hoveredNode];
    const char *typeName = NODE_NAMES[node->type];
    const char *tooltip;
    switch (node->type) {
    case NODE_COMBAT:
      tooltip = TextFormat("Combat (Floor %d)", node->layer + 1);
      break;
    case NODE_ELITE:
      tooltip = TextFormat("Elite Fight (Floor %d)", node->layer + 1);
      break;
    case NODE_BOSS:
      tooltip = TextFormat("ACT BOSS (Floor %d)", node->layer + 1);
      break;
    case NODE_SHOP:
      tooltip = TextFormat("Shop - Buy abilities (Floor %d)", node->layer + 1);
      break;
    case NODE_REST:
      tooltip = TextFormat("Rest - Heal 30%% HP (Floor %d)", node->layer + 1);
      break;
    case NODE_EVENT:
      tooltip = TextFormat("? Event (Floor %d)", node->layer + 1);
      break;
    default:
      tooltip = typeName;
      break;
    }
    int ttw = MeasureText(tooltip, 16);
    int ttx = (int)(mouse.x + 15);
    int tty = (int)(mouse.y - 10);
    if (ttx + ttw + 10 > sw)
      ttx = sw - ttw - 10;
    DrawRectangle(ttx - 4, tty - 2, ttw + 8, 20, (Color){20, 20, 30, 230});
    DrawText(tooltip, ttx, tty, 16, node->available ? WHITE : GRAY);
  }
}

MapEventType GetRandomEvent(uint32_t seed) {
  map_rng_seed(seed);
  return (MapEventType)(map_rng_next_u32() % EVENT_COUNT);
}
