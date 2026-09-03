# Terrain generation reference

This document is the implementation reference for the procedural world after
Phases 0–5. High-level engine ownership remains documented in
[`engine-architecture.md`](engine-architecture.md).

## Invariants

- Generation depends only on the world seed and world coordinates.
- `BiomeType` and `TextureType` are append-only because their ordinals are
  stored in byte-sized world and atlas data.
- Chunk cores and one-block neighbor shells must resolve terrain, caves, and
  fluids identically.
- Features with horizontal reach are evaluated from the deterministic
  `MAX_TREE_RADIUS` candidate halo.
- Biome queries (point query `getBiomeAt` and region query `getBiomeRegion`)
  return canonical biome samples for every output coordinate independently of
  display sampling step, using the canonical block-resolution erosion-aware
  pipeline with halo >= 2.
- GPU upload and destruction are not part of terrain generation and remain on
  the main thread.

## Pipeline

1. Batch the 2D terrain and climate graphs over an extended window (halo >= 2;
   chunks use 28×28 with halo=6 to cover the vegetation candidate ring).
2. Calculate continuous heights and apply one deterministic thermal-erosion
   pass at block resolution.
3. Select biomes from terrain band, temperature, humidity, weirdness, erosion,
   relief, and river values. (Point and region queries share this exact
   canonical pipeline.)
4. Produce the 16×16 core height/color data and bounded 3D noise ranges.
5. Fill voxel columns, carve caves, and assign deterministic aquifer fluids.
6. Place depth- and biome-aware ore veins.
7. Decorate caves with dripstone, wet moss, and volcanic magma.
8. Place surface and aquatic features from the cross-chunk candidate halo.
9. Generate the deterministic neighbor shell used by greedy meshing.

## FastNoise2 graphs

| Field | Graph | Octaves | Domain scale | Seed offset |
|------|-------|---------|--------------|-------------|
| Continentality | OpenSimplex2 FBm + gradient warp | 5 | 0.002 | 0 |
| Erosion | Perlin FBm | 4 | 0.004 | 1000 |
| Peaks/valleys | OpenSimplex2 FBm + gradient warp | 6 | 0.010 | 2000 |
| Mountain ridge | OpenSimplex2 ridged + gradient warp | 4 | 0.004 | 3000 |
| Temperature | OpenSimplex2 FBm | 4 | 0.0007 | 6000 |
| Humidity | OpenSimplex2 FBm | 4 | 0.0009 | 7000 |
| Weirdness | OpenSimplex2 FBm | 3 | 0.0015 | 8000 |
| River | OpenSimplex2 FBm | 2 | 0.003 | 9000 |
| Surface density | OpenSimplex2 FBm | 2 | 0.015 | 6000 |
| Cheese cave | OpenSimplex2 FBm | 2 | 0.020 | 4000 |
| Ravine | OpenSimplex2 ridged | 2 | 0.005 | 5000 |
| Spaghetti A/B | OpenSimplex2 ridged | 1 | 0.028 | 7000 / 8000 |
| Large cavern | OpenSimplex2 FBm | 1 | 0.008 | 9000 |
| Aquifer region | OpenSimplex2 FBm, 2D | 2 | 0.006 | 10000 |
| Tree variation | OpenSimplex2 FBm | 2 | 0.050 | 10000 |
| Forest density | OpenSimplex2 FBm | 3 | 0.004 | 11000 |

The spaghetti and cavern fields use a world-aligned half-resolution grid below
y=96. The grid includes a two-block halo and is reused by both core and shell
generation. This reduced the initial dense Phase 4 implementation from roughly
2.80 ms/chunk to about 1.8–1.9 ms/chunk on the Apple M1 Pro reference machine.

## Biome catalog

| Family | Biomes | Distinguishing treatment |
|--------|--------|--------------------------|
| Ocean/coast | Frozen Ocean, Ocean, Coral Reef, Beach | Ice, kelp, coral, sand/gravel/clay seabed patches |
| Cold lowlands | Snowy Tundra, Snowy Taiga, Ice Spikes | Snow caps, spruce, packed-ice spikes |
| Temperate lowlands | Plains, Flower Meadow, Forest, Birch Forest, Dark Forest | Distinct density, tint, species, and ground cover |
| Seasonal forests | Cherry Grove, Autumn Forest, Redwood Forest | Cherry crowns, mixed autumn canopy, giant 2×2 redwoods |
| Wetlands | Swamp, Mangrove Swamp | Mud/clay/moss surfaces and mangroves |
| Warm lowlands | Desert, Savanna, Jungle, Bamboo Jungle | Sandstone/cacti, acacia, jungle trees, bamboo |
| Dry/rough variants | Badlands, Moor | Terracotta mesas or coarse open heath |
| Highlands | Mountains, Snowy Mountains, Glacier | Ridges, exposed rock, global snow, packed/blue ice |
| Rivers | River, Frozen River | Stable two-block channel, clay/gravel bed, optional ice |
| Rare variants | Volcanic, Oasis, Mushroom Fields | Basalt/magma, palm pond, giant mushrooms |

Ordinary terrestrial biomes target at least 0.5% across representative
multi-seed regions. Ice Spikes, Glacier, Volcanic, Oasis, Mushroom Fields, and
Coral Reef are intentionally rare and are measured separately.

## Underground

- Medium cheese caves and ravines remain the base system.
- Two independently seeded ridged fields are ANDed into winding tunnels.
- Low-frequency rooms occur mainly below y=48.
- Aquifer water tables vary regionally between y=20 and y=36.
- The global lava table ends at y=11; volcanic pockets may reach y=24.
- Deepslate transitions between stone/tuff/deepslate below y=24.
- Coal is high-biased, iron broad, copper mid-biased, gold deep with badlands
  bonus, lapis mid-triangular, redstone/diamond deep-triangular, and emerald is
  mountain-only.

## Block and texture additions

| Phase | Appended blocks |
|------|-----------------|
| 3 | Cherry/mangrove wood and leaves, bamboo, mushroom blocks, basalt, blackstone, magma, five coral blocks |
| 4 | Lava, eight deepslate ore variants, dripstone |
| 5 | Kelp plant and kelp top |

Each block before `TextureType::COUNT` has exactly one `kBlockLayers` row and a
display name. Bundled PNGs live in `ressources/textures/`. Kelp uses transparent
cube rendering because it remains visually acceptable under water. Lily pads
and seagrass are deliberately deferred: their flat/cross-plane geometry would
be misleading as full collidable cubes.

## Adding a biome

1. Append the value before `BIOME_COUNT` in `src/utils.hpp`.
2. Append its `biomeTypeString` entry.
3. Add one `BiomeConfig` initializer in `TerrainGenerator::initBiomeConfigs`.
4. Add a structured selection path in `determineBiome`.
5. Add its map color to `GameUI::kBiomeColors`.
6. Add or reuse a deterministic feature placer without exceeding
   `MAX_TREE_RADIUS`.
7. Extend occurrence, configuration, feature, and border tests.
8. Run multi-seed histograms and document whether the biome is ordinary or
   intentionally rare.

## Adding a block and texture

1. Append the value immediately before `TextureType::COUNT`.
2. Append its `textureTypeString` display name.
3. Append the matching `kBlockLayers` row in the same ordinal position.
4. Copy a compliant PNG into `ressources/textures/` and configure a guaranteed
   bundled fallback.
5. Update transparency, face remapping, foliage/ice/host, collision, and
   emissive policy helpers as applicable.
6. Update generation and the bundled-resource audit.
7. Build the runtime and verify the reported texture-array layer count.

## Calibration and validation tools

```bash
# Biome distribution without generating voxels
./build-vk/tests/test_terrain --histogram 8192 16 1337

# Height distribution and generation throughput
./build-vk/tests/test_terrain --height-histogram 64

# Per-stage terrain profile
./build-vk/tests/test_terrain --profile 32 1337

# Multi-seed world report: biomes, heights, features, caves, and ores
./build-vk/tests/test_terrain --world-stats 16 3

# Real Vulkan streaming/LOD benchmark; saves docs/benchmarks/bench_*.txt and exits
./build-vk/ft_vox --seed 42 --benchmark 30
```

The regular `test_terrain` run additionally checks multi-thread and generation
order determinism, chunk-border continuity, aquatic features, cave fluids,
ore ranges, feature bounds, bedrock, block identifiers, and cross-API
biome consistency.

## Biome Query Invariants

Biome sampling is unified across the engine via a canonical erosion-aware pipeline:
1. Sample the 2D terrain and climate noise graphs over the requested region padded with an erosion halo (minimum 2 cells) at block resolution (`erosionStep = 1.0f`).
2. Calculate continuous heights via `calculateHeightFloat`.
3. Apply deterministic thermal erosion (`applyCanonicalErosion`) at block resolution (`step = 1.0`).
4. Select biomes using `determineBiome` with post-erosion height and clamped parameters.

- **Point queries (`getBiomeAt(worldX, worldZ)`)**: Evaluates a 5×5 cell window (halo = 2) centered on the world column via `evaluateBiomeColumn`, running thermal erosion on the neighborhood. This guarantees bit-for-bit equivalence with chunk generation without heap allocations.
- **Region queries (`getBiomeRegion(centerX, centerZ, step, width, height, ...)`)**: Returns canonical biome samples for every output coordinate, independently of display sampling step:
  - For `step == 1.0` (and small dense domains): Evaluates the dense block bounding box at `step = 1.0` with halo = 2 in a single vectorized pass.
  - For `step < 1.0` (zoom-in): Evaluates the compact dense block bounding box at `step = 1.0`. For sub-block pixels, coordinates discretize via round-to-nearest (`col = round(sampleWorldCoord)`), ensuring adjacent pixels on the same column resolve identically.
  - For `step > 1.0` (zoom-out): Uses a memory-bounded adaptive tiled approach (tiles of up to 32×32 output pixels) or direct canonical column evaluation, guaranteeing strictly bounded scratch memory (< 1 MB) without memory explosions at wide zoom-outs.
- **UI consistency**: HUD biome and World / Biome map use the same canonical generated-column biome definition as loaded terrain at every zoom level.

