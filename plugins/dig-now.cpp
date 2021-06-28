/*
 * Simulates completion of dig designations.
 */

#include "dig-now.h"

#include "DataFuncs.h"
#include "PluginManager.h"
#include "TileTypes.h"
#include "LuaTools.h"

#include "modules/Gui.h"
#include "modules/Maps.h"
#include "modules/MapCache.h"
#include "modules/Random.h"
#include "modules/Units.h"
#include "modules/World.h"

#include <df/historical_entity.h>
#include <df/map_block.h>
#include <df/reaction_product_itemst.h>
#include <df/tile_designation.h>
#include <df/tile_occupancy.h>
#include <df/ui.h>
#include <df/unit.h>
#include <df/vermin.h>
#include <df/world.h>
#include <df/world_site.h>

DFHACK_PLUGIN("dig-now");
REQUIRE_GLOBAL(ui);
REQUIRE_GLOBAL(world);

using namespace DFHack;

struct boulder_percent_options {
    // percent chance ([0..100]) for creating a boulder for the given rock type
    uint32_t layer;
    uint32_t vein;
    uint32_t small_cluster;
    uint32_t deep;

    // defaults from
    // https://dwarffortresswiki.org/index.php/DF2014:Mining
    boulder_percent_options() :
            layer(25), vein(33), small_cluster(100), deep(100) { }

    static struct_identity _identity;
};
static const struct_field_info boulder_percent_options_fields[] = {
    { struct_field_info::PRIMITIVE, "layer",         offsetof(boulder_percent_options, layer),         &df::identity_traits<uint32_t>::identity, 0, 0 },
    { struct_field_info::PRIMITIVE, "vein",          offsetof(boulder_percent_options, vein),          &df::identity_traits<uint32_t>::identity, 0, 0 },
    { struct_field_info::PRIMITIVE, "small_cluster", offsetof(boulder_percent_options, small_cluster), &df::identity_traits<uint32_t>::identity, 0, 0 },
    { struct_field_info::PRIMITIVE, "deep",          offsetof(boulder_percent_options, deep),          &df::identity_traits<uint32_t>::identity, 0, 0 },
    { struct_field_info::END }
};
struct_identity boulder_percent_options::_identity(sizeof(boulder_percent_options), &df::allocator_fn<boulder_percent_options>, NULL, "boulder_percents", NULL, boulder_percent_options_fields);

struct dig_now_options {
    bool help; // whether to show the short help

    DFCoord start; // upper-left coordinate, min z-level
    DFCoord end;   // lower-right coordinate, max z-level

    boulder_percent_options boulder_percents;

    // if set to the pos of a walkable tile (or somewhere above such a tile),
    // will dump generated boulders at this position instead of at their dig
    // locations
    DFCoord dump_pos;

    static DFCoord getMapSize() {
        uint32_t endx, endy, endz;
        Maps::getTileSize(endx, endy, endz);
        return DFCoord(endx, endy, endz);
    }

    dig_now_options() : help(false), start(0, 0, 0), end(getMapSize()) { }

    static struct_identity _identity;
};
static const struct_field_info dig_now_options_fields[] = {
    { struct_field_info::PRIMITIVE, "help",             offsetof(dig_now_options, help),             &df::identity_traits<bool>::identity, 0, 0 },
    { struct_field_info::SUBSTRUCT, "start",            offsetof(dig_now_options, start),            &df::coord::_identity,                0, 0 },
    { struct_field_info::SUBSTRUCT, "end",              offsetof(dig_now_options, end),              &df::coord::_identity,                0, 0 },
    { struct_field_info::SUBSTRUCT, "boulder_percents", offsetof(dig_now_options, boulder_percents), &boulder_percent_options::_identity,  0, 0 },
    { struct_field_info::SUBSTRUCT, "dump_pos",         offsetof(dig_now_options, dump_pos),         &df::coord::_identity,                0, 0 },
    { struct_field_info::END }
};
struct_identity dig_now_options::_identity(sizeof(dig_now_options), &df::allocator_fn<dig_now_options>, NULL, "dig_now_options", NULL, dig_now_options_fields);

// propagate light, outside, and subterranean flags to open tiles below this one
static void propagate_vertical_flags(MapExtras::MapCache &map,
                                     const DFCoord &pos) {
    df::tile_designation td = map.designationAt(pos);

    if (!map.ensureBlockAt(DFCoord(pos.x, pos.y, pos.z+1))) {
        // only the sky above
        td.bits.light = true;
        td.bits.outside = true;
        td.bits.subterranean = false;
    }

    int32_t zlevel = pos.z;
    df::tiletype_shape shape =
            tileShape(map.tiletypeAt(DFCoord(pos.x, pos.y, zlevel)));
    while ((shape == df::tiletype_shape::EMPTY
            || shape == df::tiletype_shape::RAMP_TOP)
           && map.ensureBlockAt(DFCoord(pos.x, pos.y, --zlevel))) {
        DFCoord pos_below(pos.x, pos.y, zlevel);
        df::tile_designation td_below = map.designationAt(pos_below);
        if (td_below.bits.light == td.bits.light
                && td_below.bits.outside == td.bits.outside
                && td_below.bits.subterranean == td.bits.subterranean)
            break;
        td_below.bits.light = td.bits.light;
        td_below.bits.outside = td.bits.outside;
        td_below.bits.subterranean = td.bits.subterranean;
        map.setDesignationAt(pos_below, td_below);
        shape = tileShape(map.tiletypeAt(pos_below));
    }
}

static bool can_dig_default(df::tiletype tt) {
    df::tiletype_shape shape = tileShape(tt);
    return shape == df::tiletype_shape::WALL ||
        shape == df::tiletype_shape::FORTIFICATION ||
        shape == df::tiletype_shape::RAMP ||
        shape == df::tiletype_shape::STAIR_UP ||
        shape == df::tiletype_shape::STAIR_UPDOWN;
}

static bool can_dig_channel(df::tiletype tt) {
    df::tiletype_shape shape = tileShape(tt);
    return shape != df::tiletype_shape::EMPTY &&
        shape != df::tiletype_shape::ENDLESS_PIT &&
        shape != df::tiletype_shape::NONE &&
        shape != df::tiletype_shape::RAMP_TOP &&
        shape != df::tiletype_shape::TRUNK_BRANCH;
}

static bool can_dig_up_stair(df::tiletype tt) {
    df::tiletype_shape shape = tileShape(tt);
    return shape == df::tiletype_shape::WALL ||
        shape == df::tiletype_shape::FORTIFICATION;
}

static bool can_dig_down_stair(df::tiletype tt) {
    df::tiletype_shape shape = tileShape(tt);
    return shape == df::tiletype_shape::BOULDER ||
        shape == df::tiletype_shape::BROOK_BED ||
        shape == df::tiletype_shape::BROOK_TOP ||
        shape == df::tiletype_shape::FLOOR ||
        shape == df::tiletype_shape::FORTIFICATION ||
        shape == df::tiletype_shape::PEBBLES ||
        shape == df::tiletype_shape::RAMP ||
        shape == df::tiletype_shape::SAPLING ||
        shape == df::tiletype_shape::SHRUB ||
        shape == df::tiletype_shape::TWIG ||
        shape == df::tiletype_shape::WALL;
}

static bool can_dig_up_down_stair(df::tiletype tt) {
    df::tiletype_shape shape = tileShape(tt);
    return shape == df::tiletype_shape::WALL ||
        shape == df::tiletype_shape::FORTIFICATION ||
        shape == df::tiletype_shape::STAIR_UP;
}

static bool can_dig_ramp(df::tiletype tt) {
    df::tiletype_shape shape = tileShape(tt);
    return shape == df::tiletype_shape::WALL ||
        shape == df::tiletype_shape::FORTIFICATION;
}

static void dig_type(MapExtras::MapCache &map, const DFCoord &pos,
                     df::tiletype tt) {
    auto blk = map.BlockAtTile(pos);
    if (!blk)
        return;

    // ensure we run this even if one of the later steps fails (e.g. OpenSpace)
    map.setTiletypeAt(pos, tt);

    // digging a tile reverts it to the layer soil/stone material
    if (!blk->setStoneAt(pos, tt, map.layerMaterialAt(pos)) &&
            !blk->setSoilAt(pos, tt, map.layerMaterialAt(pos)))
        return;

    // un-smooth dug tiles
    tt = map.tiletypeAt(pos);
    tt = findTileType(tileShape(tt), tileMaterial(tt), tileVariant(tt),
                      df::tiletype_special::NORMAL, tileDirection(tt));
    map.setTiletypeAt(pos, tt);
}

static void dig_shape(MapExtras::MapCache &map, const DFCoord &pos,
                      df::tiletype tt, df::tiletype_shape shape) {
    dig_type(map, pos, findSimilarTileType(tt, shape));
}

static void remove_ramp_top(MapExtras::MapCache &map, const DFCoord &pos) {
    if (!map.ensureBlockAt(pos))
        return;

    if (tileShape(map.tiletypeAt(pos)) == df::tiletype_shape::RAMP_TOP)
        dig_type(map, pos, df::tiletype::OpenSpace);
}

static bool is_wall(MapExtras::MapCache &map, const DFCoord &pos) {
    if (!map.ensureBlockAt(pos))
        return false;
    return tileShape(map.tiletypeAt(pos)) == df::tiletype_shape::WALL;
}

static void clean_ramp(MapExtras::MapCache &map, const DFCoord &pos) {
    if (!map.ensureBlockAt(pos))
        return;

    df::tiletype tt = map.tiletypeAt(pos);
    if (tileShape(tt) != df::tiletype_shape::RAMP)
        return;

    if (is_wall(map, DFCoord(pos.x-1, pos.y, pos.z)) ||
            is_wall(map, DFCoord(pos.x+1, pos.y, pos.z)) ||
            is_wall(map, DFCoord(pos.x, pos.y-1, pos.z)) ||
            is_wall(map, DFCoord(pos.x, pos.y+1, pos.z)))
        return;

    remove_ramp_top(map, DFCoord(pos.x, pos.y, pos.z+1));
    dig_shape(map,pos, tt, df::tiletype_shape::FLOOR);
}

// removes self and/or orthogonally adjacent ramps that are no longer adjacent
// to a wall
static void clean_ramps(MapExtras::MapCache &map, const DFCoord &pos) {
    clean_ramp(map, pos);
    clean_ramp(map, DFCoord(pos.x-1, pos.y, pos.z));
    clean_ramp(map, DFCoord(pos.x+1, pos.y, pos.z));
    clean_ramp(map, DFCoord(pos.x, pos.y-1, pos.z));
    clean_ramp(map, DFCoord(pos.x, pos.y+1, pos.z));
}

// destroys any colonies located at pos
static void destroy_colony(const DFCoord &pos) {
    auto same_pos = [&](df::vermin *colony){ return colony->pos == pos; };

    auto &colonies = world->vermin.colonies;
    auto found_colony = std::find_if(begin(colonies), end(colonies), same_pos);
    if (found_colony == end(colonies))
        return;
    colonies.erase(found_colony);

    auto &all_vermin = world->vermin.all;
    all_vermin.erase(
        std::find_if(begin(all_vermin), end(all_vermin), same_pos));
}

struct dug_tile_info {
    DFCoord pos;
    df::tiletype_material tmat;
    df::item_type itype;
    int32_t imat; // mat idx of boulder/gem potentially generated at this pos

    dug_tile_info(MapExtras::MapCache &map, const DFCoord &pos) {
        this->pos = pos;

        df::tiletype tt = map.tiletypeAt(pos);
        tmat = tileMaterial(tt);

        switch (map.BlockAtTile(pos)->veinTypeAt(pos)) {
            case df::inclusion_type::CLUSTER_ONE:
            case df::inclusion_type::CLUSTER_SMALL:
                itype = df::item_type::ROUGH;
                break;
            default:
                itype = df::item_type::BOULDER;
        }

        imat = -1;
        if (tileShape(tt) == df::tiletype_shape::WALL
                && (tmat == df::tiletype_material::STONE
                    || tmat == df::tiletype_material::MINERAL
                    || tmat == df::tiletype_material::FEATURE))
            imat = map.baseMaterialAt(pos).mat_index;
    }
};

static bool is_diggable(MapExtras::MapCache &map, const DFCoord &pos,
                        df::tiletype tt) {
    df::tiletype_material mat = tileMaterial(tt);
    switch (mat) {
    case df::tiletype_material::CONSTRUCTION:
    case df::tiletype_material::POOL:
    case df::tiletype_material::RIVER:
    case df::tiletype_material::TREE:
    case df::tiletype_material::ROOT:
    case df::tiletype_material::LAVA_STONE:
    case df::tiletype_material::MAGMA:
    case df::tiletype_material::HFS:
    case df::tiletype_material::UNDERWORLD_GATE:
        return false;
    default:
        break;
    }

    if (mat == df::tiletype_material::FEATURE) {
        // adamantine is the only is diggable feature
        t_feature feature;
        return map.BlockAtTile(pos)->GetLocalFeature(&feature)
                && feature.type == feature_type::deep_special_tube;
    }

    return true;
}

static bool dig_tile(color_ostream &out, MapExtras::MapCache &map,
                     const DFCoord &pos, df::tile_dig_designation designation,
                     std::vector<dug_tile_info> &dug_tiles) {
    df::tiletype tt = map.tiletypeAt(pos);

    if (!is_diggable(map, pos, tt))
        return false;

    df::tiletype target_type = df::tiletype::Void;
    switch(designation) {
        case df::tile_dig_designation::Default:
            if (can_dig_default(tt)) {
                df::tiletype_shape shape = tileShape(tt);
                df::tiletype_shape target_shape = df::tiletype_shape::FLOOR;
                if (shape == df::tiletype_shape::STAIR_UPDOWN)
                    target_shape = df::tiletype_shape::STAIR_DOWN;
                else if (shape == df::tiletype_shape::RAMP)
                    remove_ramp_top(map, DFCoord(pos.x, pos.y, pos.z+1));
                target_type = findSimilarTileType(tt, target_shape);
            }
            break;
        case df::tile_dig_designation::Channel:
        {
            DFCoord pos_below(pos.x, pos.y, pos.z-1);
            if (can_dig_channel(tt) && map.ensureBlockAt(pos_below)
                    && is_diggable(map, pos_below, map.tiletypeAt(pos_below))) {
                target_type = df::tiletype::OpenSpace;
                DFCoord pos_above(pos.x, pos.y, pos.z+1);
                if (map.ensureBlockAt(pos_above))
                    remove_ramp_top(map, pos_above);
                if (dig_tile(out, map, pos_below,
                             df::tile_dig_designation::Ramp, dug_tiles)) {
                    clean_ramps(map, pos_below);
                    // if we successfully dug out the ramp below, that took care
                    // of adding the ramp top here
                    return true;
                }
            }
            break;
        }
        case df::tile_dig_designation::UpStair:
            if (can_dig_up_stair(tt))
                target_type =
                        findSimilarTileType(tt, df::tiletype_shape::STAIR_UP);
            break;
        case df::tile_dig_designation::DownStair:
            if (can_dig_down_stair(tt)) {
                target_type =
                        findSimilarTileType(tt, df::tiletype_shape::STAIR_DOWN);

            }
            break;
        case df::tile_dig_designation::UpDownStair:
            if (can_dig_up_down_stair(tt)) {
                target_type =
                        findSimilarTileType(tt,
                                            df::tiletype_shape::STAIR_UPDOWN);
            }
            break;
        case df::tile_dig_designation::Ramp:
        {
            if (can_dig_ramp(tt)) {
                target_type = findSimilarTileType(tt, df::tiletype_shape::RAMP);
                DFCoord pos_above(pos.x, pos.y, pos.z+1);
                if (target_type != tt && map.ensureBlockAt(pos_above)
                        && is_diggable(map, pos, map.tiletypeAt(pos_above))) {
                    // only capture the tile info of pos_above if we didn't get
                    // here via the Channel case above
                    if (dug_tiles.size() == 0)
                        dug_tiles.push_back(dug_tile_info(map, pos_above));
                    destroy_colony(pos_above);
                    // set tile type directly instead of calling dig_shape
                    // because we need to use *this* tile's material, not the
                    // material of the tile above
                    map.setTiletypeAt(pos_above,
                        findSimilarTileType(tt, df::tiletype_shape::RAMP_TOP));
                    remove_ramp_top(map, DFCoord(pos.x, pos.y, pos.z+2));
                }
            }
            break;
        }
        case df::tile_dig_designation::No:
        default:
            out.printerr(
                "unhandled dig designation for tile (%d, %d, %d): %d\n",
                pos.x, pos.y, pos.z, designation);
    }

    // fail if unhandled or no change to tile
    if (target_type == df::tiletype::Void || target_type == tt)
        return false;

    dug_tiles.push_back(dug_tile_info(map, pos));
    dig_type(map, pos, target_type);

    // let light filter down to newly exposed tiles
    propagate_vertical_flags(map, pos);

    return true;
}

static bool is_smooth_wall(MapExtras::MapCache &map, const DFCoord &pos) {
    df::tiletype tt = map.tiletypeAt(pos);
    return tileSpecial(tt) == df::tiletype_special::SMOOTH
                && tileShape(tt) == df::tiletype_shape::WALL;
}

// adds adjacent smooth walls to the given tdir
static TileDirection get_adjacent_smooth_walls(MapExtras::MapCache &map,
                                               const DFCoord &pos,
                                               TileDirection tdir) {
    if (is_smooth_wall(map, DFCoord(pos.x, pos.y-1, pos.z)))
        tdir.north = 1;
    if (is_smooth_wall(map, DFCoord(pos.x, pos.y+1, pos.z)))
        tdir.south = 1;
    if (is_smooth_wall(map, DFCoord(pos.x-1, pos.y, pos.z)))
        tdir.west = 1;
    if (is_smooth_wall(map, DFCoord(pos.x+1, pos.y, pos.z)))
        tdir.east = 1;
    return tdir;
}

// ensure we have at least two directions enabled so we can find a matching
// tiletype
static TileDirection ensure_valid_tdir(TileDirection tdir) {
    if (tdir.sum() < 2) {
        if (tdir.north) tdir.south = 1;
        else if (tdir.south) tdir.north = 1;
        else if (tdir.east) tdir.west = 1;
        else if (tdir.west) tdir.east = 1;
    }
    return tdir;
}

// connects adjacent smooth walls to our new smooth wall
static bool adjust_smooth_wall_dir(MapExtras::MapCache &map,
                                   const DFCoord &pos,
                                   TileDirection tdir) {
    if (!is_smooth_wall(map, pos))
        return false;

    tdir = ensure_valid_tdir(get_adjacent_smooth_walls(map, pos, tdir));

    df::tiletype tt = map.tiletypeAt(pos);
    tt = findTileType(tileShape(tt), tileMaterial(tt), tileVariant(tt),
                      tileSpecial(tt), tdir);
    if (tt == df::tiletype::Void)
        return false;

    map.setTiletypeAt(pos, tt);
    return true;
}

// assumes that if the game let you designate a tile for smoothing, it must be
// valid to do so.
static bool smooth_tile(color_ostream &out, MapExtras::MapCache &map,
                        const DFCoord &pos) {
    df::tiletype tt = map.tiletypeAt(pos);

    TileDirection tdir;
    if (tileShape(tt) == df::tiletype_shape::WALL) {
        if (adjust_smooth_wall_dir(map, DFCoord(pos.x, pos.y-1, pos.z),
                                   TileDirection(0, 1, 0, 0)))
            tdir.north = 1;
        if (adjust_smooth_wall_dir(map, DFCoord(pos.x, pos.y+1, pos.z),
                                TileDirection(1, 0, 0, 0)))
            tdir.south = 1;
        if (adjust_smooth_wall_dir(map, DFCoord(pos.x-1, pos.y, pos.z),
                                TileDirection(0, 0, 0, 1)))
            tdir.west = 1;
        if (adjust_smooth_wall_dir(map, DFCoord(pos.x+1, pos.y, pos.z),
                                TileDirection(0, 0, 1, 0)))
            tdir.east = 1;
        tdir = ensure_valid_tdir(tdir);
    }

    tt = findTileType(tileShape(tt), tileMaterial(tt), tileVariant(tt),
                      df::tiletype_special::SMOOTH, tdir);
    if (tt == df::tiletype::Void)
        return false;

    map.setTiletypeAt(pos, tt);
    return true;
}

// assumes that if the game let you designate a tile for track carving, it must
// be valid to do so.
static bool carve_tile(MapExtras::MapCache &map,
                       const DFCoord &pos, df::tile_occupancy &to) {
    df::tiletype tt = map.tiletypeAt(pos);
    TileDirection tdir = tileDirection(tt);

    if (to.bits.carve_track_north)
        tdir.north = 1;
    if (to.bits.carve_track_east)
        tdir.east = 1;
    if (to.bits.carve_track_south)
        tdir.south = 1;
    if (to.bits.carve_track_west)
        tdir.west = 1;

    tt = findTileType(tileShape(tt), tileMaterial(tt), tileVariant(tt),
                      df::tiletype_special::TRACK, tdir);
    if (tt == df::tiletype::Void)
        return false;

    map.setTiletypeAt(pos, tt);
    return true;
}

static bool produces_item(const boulder_percent_options &options,
                          MapExtras::MapCache &map, Random::MersenneRNG &rng,
                          const dug_tile_info &info) {
    uint32_t probability;
    if (info.tmat == df::tiletype_material::FEATURE)
        probability = options.deep;
    else {
        switch (map.BlockAtTile(info.pos)->veinTypeAt(info.pos)) {
            case df::inclusion_type::CLUSTER:
            case df::inclusion_type::VEIN:
                probability = options.vein;
                break;
            case df::inclusion_type::CLUSTER_ONE:
            case df::inclusion_type::CLUSTER_SMALL:
                probability = options.small_cluster;
                break;
            default:
                probability = options.layer;
                break;
        }
    }

    return rng.random(100) < probability;
}

typedef std::map<std::pair<df::item_type, int32_t>, std::vector<DFCoord>>
    item_coords_t;

static void do_dig(color_ostream &out, std::vector<DFCoord> &dug_coords,
                   item_coords_t &item_coords, const dig_now_options &options) {
    MapExtras::MapCache map;
    Random::MersenneRNG rng;

    rng.init();

    // go down levels instead of up so stacked ramps behave as expected
    for (int16_t z = options.end.z; z >= options.start.z; --z) {
        for (int16_t y = options.start.y; y <= options.end.y; ++y) {
            for (int16_t x = options.start.x; x <= options.end.x; ++x) {
                // this will return NULL if the map block hasn't been allocated
                // yet, but that means there aren't any designations anyway.
                if (!Maps::getTileBlock(x, y, z))
                    continue;

                DFCoord pos(x, y, z);
                df::tile_designation td = map.designationAt(pos);
                df::tile_occupancy to = map.occupancyAt(pos);
                if (td.bits.dig != df::tile_dig_designation::No &&
                        !to.bits.dig_marked) {
                    std::vector<dug_tile_info> dug_tiles;
                    if (dig_tile(out, map, pos, td.bits.dig, dug_tiles)) {
                        td = map.designationAt(pos);
                        td.bits.dig = df::tile_dig_designation::No;
                        map.setDesignationAt(pos, td);
                        for (auto info : dug_tiles) {
                            dug_coords.push_back(info.pos);
                            if (info.imat < 0)
                                continue;
                            if (produces_item(options.boulder_percents,
                                              map, rng, info)) {
                                auto k = std::make_pair(info.itype, info.imat);
                                item_coords[k].push_back(info.pos);
                            }
                        }
                    }
                } else if (td.bits.smooth == 1) {
                    if (smooth_tile(out, map, pos)) {
                        to = map.occupancyAt(pos);
                        td.bits.smooth = 0;
                        map.setDesignationAt(pos, td);
                    }
                } else if (to.bits.carve_track_north == 1
                                || to.bits.carve_track_east == 1
                                || to.bits.carve_track_south == 1
                                || to.bits.carve_track_west == 1) {
                    if (carve_tile(map, pos, to)) {
                        to = map.occupancyAt(pos);
                        to.bits.carve_track_north = 0;
                        to.bits.carve_track_east = 0;
                        to.bits.carve_track_south = 0;
                        to.bits.carve_track_west = 0;
                        map.setOccupancyAt(pos, to);
                    }
                }
            }
        }
    }

    map.WriteAll();
}

// if pos is empty space, teleport to a floor somewhere below
// if we fall out of the world (e.g. empty space or walls all the way down),
// returned position will be invalid
static DFCoord simulate_fall(const DFCoord &pos) {
    DFCoord resting_pos(pos);

    while (Maps::ensureTileBlock(resting_pos)) {
        df::tiletype tt = *Maps::getTileType(resting_pos);
        df::tiletype_shape_basic basic_shape = tileShapeBasic(tileShape(tt));
        if (isWalkable(tt) && basic_shape != df::tiletype_shape_basic::Open)
            break;
        --resting_pos.z;
    }

    return resting_pos;
}

static void create_boulders(color_ostream &out,
                const item_coords_t &item_coords,
                const dig_now_options &options) {
    df::unit *unit = world->units.active[0];
    df::historical_entity *civ = df::historical_entity::find(unit->civ_id);
    df::world_site *site = World::isFortressMode() ?
            df::world_site::find(ui->site_id) : NULL;

    std::vector<df::reaction_reagent *> in_reag;
    std::vector<df::item *> in_items;

    DFCoord dump_pos;
    if (Maps::isValidTilePos(options.dump_pos)) {
        dump_pos = simulate_fall(options.dump_pos);
        if (!Maps::ensureTileBlock(dump_pos))
            out.printerr("Invalid dump tile coordinates! Ensure the --dump"
                " option specifies an open, non-wall tile.");
    }

    for (auto entry : item_coords) {
        df::reaction_product_itemst *prod =
                df::allocate<df::reaction_product_itemst>();
        const std::vector<DFCoord> &coords = entry.second;

        prod->item_type = entry.first.first;
        prod->item_subtype = -1;
        prod->mat_type = 0;
        prod->mat_index = entry.first.second;
        prod->probability = 100;
        prod->product_dimension = 1;

        std::vector<df::reaction_product*> out_products;
        std::vector<df::item *> out_items;

        size_t remaining_items = coords.size();
        while (remaining_items > 0) {
            int16_t batch_size = min(remaining_items,
                                     static_cast<size_t>(INT16_MAX));
            prod->count = batch_size;
            remaining_items -= batch_size;
            prod->produce(unit, &out_products, &out_items, &in_reag, &in_items,
                          1, job_skill::NONE, 0, civ, site, NULL);
        }

        size_t num_items = out_items.size();
        if (num_items != coords.size()) {
            MaterialInfo material;
            material.decode(prod->mat_type, prod->mat_index);
            out.printerr("unexpected number of %s %s produced: expected %zd,"
                         " got %zd.\n",
                         material.toString().c_str(),
                         ENUM_KEY_STR(item_type, prod->item_type).c_str(),
                         coords.size(), num_items);
            num_items = min(num_items, entry.second.size());
        }

        for (size_t i = 0; i < num_items; ++i) {
            DFCoord pos = Maps::isValidTilePos(dump_pos) ?
                    dump_pos : simulate_fall(coords[i]);
            if (!Maps::ensureTileBlock(pos)) {
                out.printerr(
                        "unable to place boulder generated at (%d, %d, %d)\n",
                        coords[i].x, coords[i].y, coords[i].z);
                continue;
            }
            out_items[i]->moveToGround(pos.x, pos.y, pos.z);
        }

        delete(prod);
    }
}

static void flood_unhide(color_ostream &out, const DFCoord &pos) {
    auto L = Lua::Core::State;
    Lua::StackUnwinder top(L);

    if (!lua_checkstack(L, 2)
            || !Lua::PushModulePublic(out, L, "plugins.reveal", "unhideFlood"))
        return;

    Lua::Push(L, pos);
    Lua::SafeCall(out, L, 1, 0);
}

static void post_process_dug_tiles(color_ostream &out,
                             const std::vector<DFCoord> &dug_coords) {
    for (DFCoord pos : dug_coords) {
        if (Maps::getTileDesignation(pos)->bits.hidden)
            flood_unhide(out, pos);

        df::tile_occupancy &to = *Maps::getTileOccupancy(pos);
        if (to.bits.unit || to.bits.item) {
            DFCoord resting_pos = simulate_fall(pos);
            if (resting_pos == pos)
                continue;

            if (!Maps::ensureTileBlock(resting_pos)) {
                out.printerr("No valid tile beneath (%d, %d, %d); can't move"
                             " units and items to floor",
                             pos.x, pos.y, pos.z);
                continue;
            }

            if (to.bits.unit) {
                std::vector<df::unit*> units;
                Units::getUnitsInBox(units, pos.x, pos.y, pos.z,
                                     pos.x, pos.y, pos.z);
                for (auto unit : units)
                    Units::teleport(unit, resting_pos);
            }

            if (to.bits.item) {
                for (auto item : world->items.other.IN_PLAY) {
                    if (item->pos == pos && item->flags.bits.on_ground)
                        item->moveToGround(
                                resting_pos.x, resting_pos.y, resting_pos.z);
                }
            }
        }

        // refresh block metadata and flows
        Maps::enableBlockUpdates(Maps::getTileBlock(pos), true, true);
    }
}

static bool get_options(color_ostream &out,
                        dig_now_options &opts,
                        const std::vector<std::string> &parameters) {
    auto L = Lua::Core::State;
    Lua::StackUnwinder top(L);

    if (!lua_checkstack(L, parameters.size() + 2) ||
        !Lua::PushModulePublic(
            out, L, "plugins.dig-now", "parse_commandline")) {
        out.printerr("Failed to load dig-now Lua code\n");
        return false;
    }

    Lua::Push(L, &opts);

    for (const std::string &param : parameters)
        Lua::Push(L, param);

    if (!Lua::SafeCall(out, L, parameters.size() + 1, 0))
        return false;

    return true;
}

static void print_help(color_ostream &out) {
    auto L = Lua::Core::State;
    Lua::StackUnwinder top(L);

    if (!lua_checkstack(L, 1) ||
        !Lua::PushModulePublic(out, L, "plugins.dig-now", "print_help") ||
        !Lua::SafeCall(out, L, 0, 0)) {
        out.printerr("Failed to load dig-now Lua code\n");
    }
}

bool dig_now_impl(color_ostream &out, const dig_now_options &options) {
    if (!Maps::IsValid()) {
        out.printerr("Map is not available!\n");
        return false;
    }

    // required for boulder generation
    if (world->units.active.size() == 0) {
        out.printerr("At least one unit must be alive!\n");
        return false;
    }

    // track which positions were modified and where to produce items
    std::vector<DFCoord> dug_coords;
    item_coords_t item_coords;

    do_dig(out, dug_coords, item_coords, options);
    create_boulders(out, item_coords, options);
    post_process_dug_tiles(out, dug_coords);

    // force the game to recompute its walkability cache
    world->reindex_pathfinding = true;

    return true;
}

command_result dig_now(color_ostream &out, std::vector<std::string> &params) {
    CoreSuspender suspend;

    dig_now_options options;
    if (!get_options(out, options, params) || options.help)
    {
        print_help(out);
        return options.help ? CR_OK : CR_FAILURE;
    }

    return dig_now_impl(out, options) ? CR_OK : CR_FAILURE;
}

DFhackCExport command_result plugin_init(color_ostream &,
                                         std::vector<PluginCommand> &commands) {
    commands.push_back(PluginCommand(
        "dig-now", "Instantly complete dig designations", dig_now, false));
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &) {
    return CR_OK;
}

// Lua API

// runs dig-now for the specified tile coordinate. default options apply.
static int dig_now_tile(lua_State *L)
{
    DFCoord pos;
    if (lua_gettop(L) <= 1)
        Lua::CheckDFAssign(L, &pos, 1);
    else
        pos = DFCoord(luaL_checkint(L, 1), luaL_checkint(L, 2),
                      luaL_checkint(L, 3));

    color_ostream *out = Lua::GetOutput(L);
    if (!out)
        out = &Core::getInstance().getConsole();

    return 1;

    dig_now_options options;
    options.start = pos;
    options.end = pos;
    lua_pushboolean(L, dig_now_impl(*out, options));

    return 1;
}

DFHACK_PLUGIN_LUA_COMMANDS {
    DFHACK_LUA_COMMAND(dig_now_tile),
    DFHACK_LUA_END
};
