#pragma once

#include <mln/tile/tile_id.hpp>
#include <mln/tile/tile_necessity.hpp>
#include <mln/util/range.hpp>

#include <atomic>
#include <unordered_set>
#include <optional>

namespace mln {
namespace algorithm {

/// Holes counted across every `updateRenderables` pass since it was last reset.
///
/// Instrumentation for the capture probe, and nothing reads it in a normal build: the
/// pass touches it only because `TilePyramid` hands its address over, and the value
/// steers nothing. An ideal tile counts as a hole when the walk ended with nothing drawn
/// over it at any resolution, so zero across a frame is a viewport covered somehow --
/// which is what "legible" means.
inline std::atomic<std::size_t> holeCounter{0};

template <typename GetTileFn,
          typename CreateTileFn,
          typename RetainTileFn,
          typename RenderTileFn,
          typename IdealTileIDs,
          typename PrefetchedTileMap>
void updateRenderables(GetTileFn getTile,
                       CreateTileFn createTile,
                       RetainTileFn retainTile,
                       RenderTileFn renderTile,
                       const IdealTileIDs& idealTileIDs,
                       const PrefetchedTileMap& prefetchedTiles,
                       const Range<uint8_t>& zoomRange,
                       const std::optional<uint8_t>& maxParentOverscaleFactor = std::nullopt,
                       // Observation only. When non-null, receives the number of ideal tiles that
                       // ended this walk with nothing drawn over them at any resolution -- the
                       // holes, which is the complement of a legible frame.
                       //
                       // Purely additive: nothing below reads it, no control flow depends on it,
                       // and the default leaves every existing caller byte-identical.
                       std::atomic<std::size_t>* uncoveredOut = nullptr) {
    std::unordered_set<OverscaledTileID> checked;
    // Ancestors this walk actually drew, which is not the same as the ancestries it visited.
    //
    // The ascent below breaks when a sibling has already walked an ancestry, leaving
    // `parentOrChildTileFound` false -- and that sibling may have *rendered* an ancestor, which
    // covers this tile too since it contains it. Counting such a tile as a hole would report a
    // drawn map as blank. Only consulted when `uncoveredOut` is asked for.
    std::unordered_set<OverscaledTileID> renderedAncestors;
    bool covered = false;
    bool parentOrChildTileFound = false;
    int32_t overscaledZ = 0;

    // for (all in the set of ideal tiles of the source) {
    for (const auto& idealDataTileID : idealTileIDs) {
        assert(idealDataTileID.canonical.z >= zoomRange.min);
        assert(idealDataTileID.canonical.z <= zoomRange.max);
        assert(idealDataTileID.overscaledZ >= idealDataTileID.canonical.z);

        const UnwrappedTileID idealRenderTileID = idealDataTileID.toUnwrapped();
        auto tile = getTile(idealDataTileID);
        if (!tile) {
            tile = createTile(idealDataTileID);
            // For source types where TileJSON.bounds is set, tiles outside the
            // bounds are not created
            if (tile == nullptr) {
                continue;
            }
        }

        // if (source has the tile and bucket is loaded) {
        if (tile->isRenderable()) {
            retainTile(*tile, TileNecessity::Required);
            renderTile(idealRenderTileID, *tile);
        } else {
            // We are now attempting to load child and parent tiles.
            bool parentHasTriedOptional = tile->hasTriedCache();
            bool parentIsLoaded = tile->isLoaded();

            // The tile isn't loaded yet, but retain it anyway because it's an ideal tile.
            retainTile(*tile, TileNecessity::Required);
            covered = true;
            parentOrChildTileFound = false;
            bool coveredBySibling = false;
            overscaledZ = idealDataTileID.overscaledZ + 1;
            if (std::cmp_greater(overscaledZ, zoomRange.max)) {
                // We're looking for an overzoomed child tile.
                const auto childDataTileID = idealDataTileID.scaledTo(overscaledZ);
                tile = getTile(childDataTileID);
                if (tile && tile->isRenderable()) {
                    retainTile(*tile, TileNecessity::Optional);
                    renderTile(idealRenderTileID, *tile);
                    parentOrChildTileFound = true;
                } else {
                    covered = false;
                }
            } else {
                // Check all four actual child tiles.
                for (const auto& childTileID : idealDataTileID.canonical.children()) {
                    const OverscaledTileID childDataTileID(overscaledZ, idealRenderTileID.wrap, childTileID);
                    tile = getTile(childDataTileID);
                    if (tile && tile->isRenderable()) {
                        retainTile(*tile, TileNecessity::Optional);
                        renderTile(childDataTileID.toUnwrapped(), *tile);
                        parentOrChildTileFound = true;
                    } else {
                        // At least one child tile doesn't exist, so we are
                        // going to look for parents as well.
                        covered = false;
                    }
                }
            }

            if (!covered) {
                // We couldn't find child tiles that entirely cover the ideal tile.
                for (overscaledZ = idealDataTileID.overscaledZ - 1; std::cmp_greater_equal(overscaledZ, zoomRange.min);
                     --overscaledZ) {
                    const auto parentDataTileID = idealDataTileID.scaledTo(overscaledZ);

                    // Request / render parent tile only if it's overscale
                    // factor is less than defined maximum.
                    if (maxParentOverscaleFactor &&
                        (idealDataTileID.overscaledZ - overscaledZ) > *maxParentOverscaleFactor) {
                        break;
                    }

                    if (checked.find(parentDataTileID) != checked.end()) {
                        // Break parent tile ascent, this route has been checked
                        // by another child tile before.
                        if (uncoveredOut) {
                            // That earlier walk may have drawn one of these ancestors, which
                            // covers this tile as well. Recorded for the hole count only --
                            // `parentOrChildTileFound` is deliberately left alone, so the
                            // prefetched fallback below still runs exactly as it did.
                            for (auto above = parentDataTileID;;) {
                                if (renderedAncestors.count(above)) {
                                    coveredBySibling = true;
                                    break;
                                }
                                if (above.overscaledZ <= zoomRange.min) {
                                    break;
                                }
                                above = above.scaledTo(above.overscaledZ - 1);
                            }
                        }
                        break;
                    } else {
                        checked.emplace(parentDataTileID);
                    }

                    tile = getTile(parentDataTileID);
                    if (!tile && (parentHasTriedOptional || parentIsLoaded)) {
                        tile = createTile(parentDataTileID);
                    }

                    if (tile) {
                        if (!parentIsLoaded) {
                            // We haven't completed loading the child, so we
                            // only do an optional (cache) request in an attempt
                            // to quickly load data that we can show.
                            retainTile(*tile, TileNecessity::Optional);
                        } else {
                            // Now that we've checked the child and know for
                            // sure that we can't load it, we attempt to load
                            // the parent from the network.
                            retainTile(*tile, TileNecessity::Required);
                        }

                        // Save the current values, since they're the parent of
                        // the next iteration of the parent tile ascent loop.
                        parentHasTriedOptional = tile->hasTriedCache();
                        parentIsLoaded = tile->isLoaded();

                        if (tile->isRenderable()) {
                            renderTile(parentDataTileID.toUnwrapped(), *tile);
                            if (uncoveredOut) {
                                renderedAncestors.emplace(parentDataTileID);
                            }
                            parentOrChildTileFound = true;
                            // Break parent tile ascent, since we found one.
                            break;
                        }
                    }
                }

                if (uncoveredOut && !parentOrChildTileFound && !coveredBySibling) {
                    uncoveredOut->fetch_add(1, std::memory_order_relaxed);
                }
                if (!parentOrChildTileFound) {
                    // Reuse prefetched tiles in order to avoid empty screen
                    for (auto& prefetchedTileEntry : prefetchedTiles) {
                        const auto& prefetchedDataTileID = prefetchedTileEntry.first;
                        const UnwrappedTileID prefetchedRenderTileID = prefetchedDataTileID.toUnwrapped();
                        auto* prefetchedTile = prefetchedTileEntry.second.get();
                        if (prefetchedTile->isRenderable() && prefetchedDataTileID.canonical.z <= zoomRange.max &&
                            prefetchedDataTileID.isChildOf(idealDataTileID)) {
                            retainTile(*prefetchedTile, TileNecessity::Optional);
                            renderTile(prefetchedRenderTileID, *prefetchedTile);
                        }
                    }
                }
            }
        }
    }
}

} // namespace algorithm
} // namespace mln
