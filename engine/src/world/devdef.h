/* DEVS section reader (docs/27-dev-section.md): the devices a scenario places and their tag data (machines, controls,
 * light fixtures), device groups, trigger volumes, cutscene flags and camera points, player starting locations, the
 * object-name table (script names -> SCEN placement / device), sound scenery, decals and BSP switch volumes. Written by
 * tools/halo2ps2/devconv.py, read here (host-tested, pure C11, no heap, no double); the device state machine on top of
 * it is sim/devices.[ch].
 *
 * Units: world units, radians, TICKS of 1/30 s (the tags hold seconds; the converter multiplies by 30 and rounds).
 * Cross references: tag ids (u32, Xbox ids as everywhere); `scen_index` is an index into the placements of the SCEN
 * section of the same map (kind by kind, as scenconv orders them); bsp_mask bit n = the thing lies in BSP n (point
 * location in the converter). A section that devdef_open accepted can be walked with every accessor; an accessor
 * with an index out of range returns a neutral zero record (`name_index`/`group` fields -1). */
#ifndef WORLD_DEVDEF_H
#define WORLD_DEVDEF_H
#include <stddef.h>
#include <stdint.h>

#define DEVDEF_NAME_LEN 32
#define DEVDEF_NONE 0xFFFFFFFFu            /* scen_index / aux_index: none */
#define DEVDEF_NO_ANIM 0xFFFFu
#define DEVDEF_MAX_BSPS 32

typedef enum { DEVDEF_OK = 0, DEVDEF_ERR_TRUNCATED, DEVDEF_ERR_MAGIC, DEVDEF_ERR_LAYOUT, DEVDEF_ERR_VERSION } devdef_status;

/* devdef_def.kind */
enum { DEVDEF_MACHINE = 0, DEVDEF_CONTROL = 1, DEVDEF_LIGHT = 2 };
/* machine subtype */
enum { DEVDEF_MACH_DOOR = 0, DEVDEF_MACH_PLATFORM = 1, DEVDEF_MACH_GEAR = 2 };
/* control subtype */
enum { DEVDEF_CTRL_TOGGLE = 0, DEVDEF_CTRL_ON = 1, DEVDEF_CTRL_OFF = 2, DEVDEF_CTRL_CALL = 3 };
/* devdef_def.dev_flags (device tag) */
#define DEVDEF_DF_POSITION_LOOPS 1u
#define DEVDEF_DF_NOT_INTERPOLATED 2u
/* devdef_def.kind_flags (machine tag; bit 2 occurs only on platforms) */
#define DEVDEF_MF_PATH_OBSTACLE 1u
#define DEVDEF_MF_NOT_WHEN_OPEN 2u
#define DEVDEF_MF_ELEVATOR 4u
/* devdef_device.device_flags (scenario placement) */
#define DEVDEF_PF_INITIALLY_OPEN 1u
#define DEVDEF_PF_INITIALLY_OFF 2u
#define DEVDEF_PF_CHANGE_ONCE 4u
#define DEVDEF_PF_REVERSED 8u              /* position = 1 - group value */
#define DEVDEF_PF_BOTH_SIDES 16u
/* devdef_device.kind_flags of a machine placement (names unverified except that bits 0..3 are the only ones seen) */
#define DEVDEF_MPF_NO_AUTO 1u              /* does not operate automatically */
#define DEVDEF_MPF_ONE_SIDED 2u
#define DEVDEF_MPF_NEVER_LOCKED 4u
#define DEVDEF_MPF_MELEE 8u                /* opened by melee attack */
#define DEVDEF_MPF_ONE_SIDED_PLAYER 16u    /* never seen */
#define DEVDEF_MPF_NO_AUTO_CLOSE 32u       /* never seen */
/* devdef_device.flags (SCEN placement flags) */
#define DEVDEF_P_INITIALLY_VISIBLE 1u      /* clear: created by a script */
#define DEVDEF_P_NOT_EASY 2u
#define DEVDEF_P_NOT_NORMAL 4u
#define DEVDEF_P_NOT_HARD 8u
#define DEVDEF_P_PLAYER_APPEARANCE 16u
/* devdef_group.flags */
#define DEVDEF_GF_CHANGE_ONCE 1u
/* effect / sound slots of devdef_def.fx[] */
enum { DEVDEF_FX_OPEN = 0, DEVDEF_FX_CLOSE, DEVDEF_FX_OPENED, DEVDEF_FX_CLOSED, DEVDEF_FX_DEPOWERED, DEVDEF_FX_REPOWERED,
       DEVDEF_FX_DELAY, DEVDEF_FX_CTL_ON, DEVDEF_FX_CTL_OFF, DEVDEF_FX_CTL_DENY, DEVDEF_FX_COUNT };
/* devdef_name.flags */
#define DEVDEF_NF_RESOLVED 1u              /* a placement carries this name (else the entry is dangling) */
#define DEVDEF_NF_DEVICE 2u                /* ... and it is a placed device: aux_index = device index */
#define DEVDEF_NF_AMBIENT 4u               /* ... or sound scenery: aux_index = ambient index */

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       def_count, device_count, group_count, volume_count, start_count, flag_count, camera_count,
	               name_count, ambient_count, decal_count, decal_type_count, bspsw_count, bsp_count;
	uint32_t       dropped_devices, dangling_names;
	const uint8_t *defs, *devices, *groups, *volumes, *starts, *flags, *cameras, *names, *name_order, *ambients,
	              *decals, *decal_types, *bspsw;
} devdef_section;

/* A device tag. Times are ticks. power / position transition: how long a full 0 -> 1 (or 1 -> 0) change takes; the
 * acceleration times are carried for the host's animation easing (the runtime moves linearly, docs/27). */
typedef struct {
	uint32_t tag_id, model_tag, anim_tag;
	uint8_t  kind, subtype;
	uint16_t dev_flags;                /* DEVDEF_DF_* */
	uint16_t kind_flags;               /* DEVDEF_MF_* (machines) */
	uint16_t triggers;                 /* control "triggers when" raw */
	uint32_t power_ticks, power_accel_ticks, position_ticks, position_accel_ticks, depowered_ticks, depowered_accel_ticks;
	float    auto_radius;              /* automatic activation radius, world units (doors only) */
	uint32_t delay_ticks;
	uint32_t open_ticks;               /* door: how long it stays open after the last activator left */
	uint8_t  fn_in[4];                 /* inputs A..D of the object functions: 0 none, 1 power, 2 d power, 3 position, 4 d position, 5 locked, 6 delay */
	uint32_t fx[DEVDEF_FX_COUNT];      /* tag ids (snd! or effe), 0 = none */
	uint16_t fx_snd_mask;              /* bit i: fx[i] is a snd! tag */
	uint16_t pos_anim, power_anim;     /* animation indices in anim_tag's graph (DEVDEF_NO_ANIM) */
	uint16_t pos_frames, power_frames; /* frame count of those animations: position p -> frame p * (frames - 1) */
	uint8_t  collision, elevator_node;
	float    call_value;
	char     name[DEVDEF_NAME_LEN];    /* tag path tail */
} devdef_def;

typedef struct {
	uint16_t def;                      /* index into the defs */
	uint16_t flags;                    /* DEVDEF_P_* */
	uint32_t bsp_mask;
	float    pos[3], rot[3];           /* yaw, pitch, roll */
	int16_t  power_group, position_group;   /* -1 none */
	uint32_t device_flags;             /* DEVDEF_PF_* */
	uint32_t kind_flags;               /* machine: DEVDEF_MPF_*; control: raw u32 (0 / 1, meaning unknown) */
	int16_t  name_index;               /* object name index or -1 */
	uint16_t extra;                    /* control: raw u16 (0..4, meaning unknown) */
	uint32_t scen_index;               /* index into the SCEN placements */
	float    light_color[3], light_intensity, light_falloff, light_cutoff;   /* light fixtures */
} devdef_device;

typedef struct { char name[DEVDEF_NAME_LEN]; float initial; uint32_t flags; } devdef_group;

/* Oriented box: p is inside when 0 <= dot(p - position, axis_i) <= extents[i] for i = forward, left, up
 * (left = up x forward). */
typedef struct {
	char  name[DEVDEF_NAME_LEN];
	float forward[3], left[3], up[3], position[3], extents[3];
} devdef_volume;

typedef struct {
	float   pos[3], facing;            /* facing: yaw in radians */
	int16_t team, bsp;                 /* raw tag fields (bsp is 0 on every map: use bsp_mask) */
	int16_t game_type[4];
	uint32_t bsp_mask;
} devdef_start;

typedef struct { char name[DEVDEF_NAME_LEN]; float pos[3], yaw, pitch; uint32_t bsp_mask; } devdef_cflag;
typedef struct { char name[DEVDEF_NAME_LEN]; float pos[3], ori[3] /* yaw, pitch, roll */, fov /* radians */; uint32_t bsp_mask; } devdef_camera;

typedef struct {
	char     name[DEVDEF_NAME_LEN];
	uint8_t  obj_type;                 /* the tag's object type: 0 biped 1 vehicle 2 weapon 3 equipment 6 scenery 7 machine 8 control 9 light fixture 11 sound scenery */
	uint8_t  scen_type;                /* scen_type of the object (world/scen.h) */
	uint8_t  flags;                    /* DEVDEF_NF_* */
	uint16_t table_index;              /* raw: index in the scenario's own placement block of that kind */
	uint32_t scen_index;               /* SCEN placement index or DEVDEF_NONE (dangling name, sound scenery, dropped placement) */
	uint32_t aux_index;                /* device index (NF_DEVICE) / ambient index (NF_AMBIENT) / DEVDEF_NONE */
} devdef_name;

typedef struct {
	uint32_t ssce_tag, sound_tag;      /* sound_scenery tag, its sound_looping tag */
	float    pos[3], rot[3];
	int16_t  name_index;
	uint16_t flags;                    /* DEVDEF_P_* */
	uint32_t bsp_mask;
	uint32_t raw[2];
} devdef_ambient;

/* A decal: direction (cos p cos y, cos p sin y, sin p) is the surface normal; type indexes devdef_decal_type(). */
typedef struct { uint16_t type; int8_t yaw_raw, pitch_raw; float pos[3], yaw, pitch; uint32_t bsp_mask; } devdef_decal;
typedef struct { uint16_t volume, source, dest, unknown; } devdef_bspsw;

/* Validates the whole section once; a failed open leaves `s` zeroed (accessors then return empty records). */
devdef_status devdef_open(devdef_section *s, const uint8_t *data, size_t size);

devdef_def     devdef_get_def(const devdef_section *s, uint32_t index);
int            devdef_find_def(const devdef_section *s, uint32_t tag_id);              /* binary search, -1 absent */
devdef_device  devdef_get_device(const devdef_section *s, uint32_t index);
int            devdef_find_device_by_scen(const devdef_section *s, uint32_t scen_index);   /* binary search, -1 */
devdef_group   devdef_get_group(const devdef_section *s, uint32_t index);
devdef_volume  devdef_get_volume(const devdef_section *s, uint32_t index);
devdef_start   devdef_get_start(const devdef_section *s, uint32_t index);
devdef_cflag   devdef_get_cflag(const devdef_section *s, uint32_t index);
devdef_camera  devdef_get_camera(const devdef_section *s, uint32_t index);
devdef_name    devdef_get_name(const devdef_section *s, uint32_t index);
devdef_ambient devdef_get_ambient(const devdef_section *s, uint32_t index);
devdef_decal   devdef_get_decal(const devdef_section *s, uint32_t index);
uint32_t       devdef_decal_type(const devdef_section *s, uint32_t type);               /* deca tag id, 0 none */
devdef_bspsw   devdef_get_bspsw(const devdef_section *s, uint32_t index);

/* Lookups by script name (exact, case sensitive): -1 when absent. Object names use the sorted order table. */
int devdef_find_name(const devdef_section *s, const char *name);
int devdef_find_group(const devdef_section *s, const char *name);
int devdef_find_volume(const devdef_section *s, const char *name);
int devdef_find_cflag(const devdef_section *s, const char *name);
int devdef_find_camera(const devdef_section *s, const char *name);
/* Object name -> placed device index / SCEN placement index (-1 when the name has no such placement). */
int devdef_name_to_device(const devdef_section *s, uint32_t name_index);
int devdef_name_to_scen(const devdef_section *s, uint32_t name_index);
/* The BSP switch entry of a trigger volume, or -1. */
int devdef_find_bspsw_by_volume(const devdef_section *s, uint32_t volume);

#endif
