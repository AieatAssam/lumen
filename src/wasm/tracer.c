/*
 * Lumen — Physically-Based Path Tracer (WASM core)
 *
 * Compile with:
 *   emcc tracer.c -O3 -s WASM=1 \
 *     -s EXPORTED_FUNCTIONS='["_init","_render","_get_pixels","_set_camera","_look_at","_get_total_samples","_malloc","_free"]' \
 *     -s ALLOW_MEMORY_GROWTH=1 -s TOTAL_MEMORY=256MB \
 *     -o tracer.js
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

/* ── Math helpers ── */
#define PI 3.14159265359f
#define INF 1e30f
#define MAX_DEPTH 8
#define RR_DEPTH 3

static inline float randf(unsigned int *seed) {
    *seed = (*seed * 1103515245u + 12345u) & 0x7fffffffu;
    return (float)(*seed) / 2147483648.0f;
}

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }

/* ── Vec3 ── */
typedef struct { float x, y, z; } Vec3;

static inline Vec3 v3(float x, float y, float z) {
    Vec3 v = {x, y, z}; return v;
}
static inline Vec3 v3_add(Vec3 a, Vec3 b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline Vec3 v3_mul(Vec3 a, float s) { return v3(a.x*s, a.y*s, a.z*s); }
static inline Vec3 v3_mulv(Vec3 a, Vec3 b) { return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static inline float v3_dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3 v3_cross(Vec3 a, Vec3 b) {
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline float v3_len(Vec3 v) { return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }
static inline Vec3 v3_norm(Vec3 v) {
    float l = v3_len(v);
    if (l < 1e-8f) return v3(0,1,0);
    return v3_mul(v, 1.0f/l);
}
static inline Vec3 v3_lerp(Vec3 a, Vec3 b, float t) {
    return v3_add(v3_mul(a, 1.0f-t), v3_mul(b, t));
}

/* ── Ray ── */
typedef struct { Vec3 origin; Vec3 dir; } Ray;

/* ── Material types ── */
#define MAT_LAMBERTIAN 0
#define MAT_METAL      1
#define MAT_DIELECTRIC 2
#define MAT_EMISSIVE   3

typedef struct {
    Vec3 albedo;
    Vec3 emission;
    float roughness;
    float ior;
    int type;
} Material;

/* ── Sphere ── */
typedef struct {
    Vec3 center;
    float radius;
    int mat_id;
} Sphere;

/* ── Camera ── */
typedef struct {
    Vec3 eye;
    Vec3 lookat;
    Vec3 up;
    float fov;
    float aperture;
    float focus_dist;
} Camera;

/* ── Hit record ── */
typedef struct {
    float t;
    Vec3 point;
    Vec3 normal;
    int mat_id;
    int front_face;
} Hit;

/* ── Global state ── */
static int g_width = 0;
static int g_height = 0;
static float *g_accum = NULL;
static unsigned char *g_output = NULL;
static Sphere *g_spheres = NULL;
static Material *g_materials = NULL;
static int g_num_spheres = 0;
static int g_cosmic_backdrop_idx = -1;  /* sphere index of Cosmic backdrop, skipped when HDRI active */
static int g_num_materials = 0;
static Camera g_camera;
static int g_scene_id = 0;
static int g_total_samples = 0;

/* ── Environment map ── */
static float *g_env_map = NULL;
static int g_env_w = 0;
static int g_env_h = 0;
static int g_use_env_map = 0;  /* 0 = procedural sky, 1 = env map */

/* ── Scene declarations ── */
static void setup_cornell_box(void);
static void setup_metal_spheres(void);
static void setup_glass_light(void);
static void setup_random_spheres(void);
static void setup_checkerboard(void);
static void setup_cosmic(void);

static void free_scene(void) {
    if (g_spheres) { free(g_spheres); g_spheres = NULL; }
    if (g_materials) { free(g_materials); g_materials = NULL; }
    g_num_spheres = 0;
    g_num_materials = 0;
    g_cosmic_backdrop_idx = -1;
    /* Don't free g_env_map here — it's managed separately via load_env_map */
}

static int add_material(Vec3 albedo, Vec3 emission, float roughness, float ior, int type) {
    g_materials = realloc(g_materials, (g_num_materials + 1) * sizeof(Material));
    int idx = g_num_materials++;
    g_materials[idx].albedo = albedo;
    g_materials[idx].emission = emission;
    g_materials[idx].roughness = roughness;
    g_materials[idx].ior = ior;
    g_materials[idx].type = type;
    return idx;
}

static void add_sphere(Vec3 center, float radius, int mat_id) {
    g_spheres = realloc(g_spheres, (g_num_spheres + 1) * sizeof(Sphere));
    g_spheres[g_num_spheres].center = center;
    g_spheres[g_num_spheres].radius = radius;
    g_spheres[g_num_spheres].mat_id = mat_id;
    g_num_spheres++;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scene 0: Cornell Box — nested refraction showcase
 *
 * NESTED SPHERES: Placing a small sphere at the SAME center as a larger
 * glass sphere creates a "glass marble" effect — light refracts through
 * the outer glass shell, hits the inner core, then refracts back out.
 * This produces dramatic Fresnel reflections on the outer surface with
 * the inner material visible through the glass.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void setup_cornell_box(void) {
    int white = add_material(v3(0.95,0.93,0.88), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int red   = add_material(v3(0.70,0.06,0.06), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int green = add_material(v3(0.06,0.48,0.10), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int floor_gray = add_material(v3(0.4,0.4,0.45), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int floor_dark = add_material(v3(0.13,0.13,0.16), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int light = add_material(v3(1,1,1), v3(15,15,15), 0, 1, MAT_EMISSIVE);
    /* Metals with realistic names and albedos — full roughness gradient */
    int chrome     = add_material(v3(0.97,0.95,0.92), v3(0,0,0), 0.0f,  1, MAT_METAL);
    int silver     = add_material(v3(0.92,0.90,0.87), v3(0,0,0), 0.04f, 1, MAT_METAL);
    int nickel     = add_material(v3(0.80,0.78,0.72), v3(0,0,0), 0.12f, 1, MAT_METAL);
    int copper     = add_material(v3(0.95,0.54,0.28), v3(0,0,0), 0.06f, 1, MAT_METAL);
    int steel      = add_material(v3(0.55,0.53,0.50), v3(0,0,0), 0.18f, 1, MAT_METAL);
    int brushed_iron = add_material(v3(0.62,0.58,0.55), v3(0,0,0), 0.22f, 1, MAT_METAL);
    /* Dielectrics — strongly tinted for visible color in both reflection and refraction */
    int glass      = add_material(v3(0.97,0.97,0.97), v3(0,0,0), 0, 1.50f, MAT_DIELECTRIC);
    int diamond    = add_material(v3(1.0,1.0,1.0), v3(0,0,0), 0, 2.42f, MAT_DIELECTRIC);
    int sapphire   = add_material(v3(0.35,0.50,1.0), v3(0,0,0), 0, 1.77f, MAT_DIELECTRIC);
    int emerald    = add_material(v3(0.15,0.92,0.30), v3(0,0,0), 0, 1.58f, MAT_DIELECTRIC);
    int ruby       = add_material(v3(0.95,0.08,0.15), v3(0,0,0), 0, 1.54f, MAT_DIELECTRIC);
    int amber      = add_material(v3(1.0,0.65,0.08), v3(0,0,0), 0, 1.55f, MAT_DIELECTRIC);
    /* Inner core materials for nested spheres */
    int core_warm  = add_material(v3(1.0,0.9,0.3), v3(4,2.5,0.5), 0, 1, MAT_EMISSIVE);
    int core_cool  = add_material(v3(0.3,0.6,1.0), v3(0.5,2,6), 0, 1, MAT_EMISSIVE);
    int core_green = add_material(v3(0.2,1.0,0.3), v3(0.3,3,0.5), 0, 1, MAT_EMISSIVE);
    int core_red   = add_material(v3(1.0,0.2,0.2), v3(5,0.5,0.3), 0, 1, MAT_EMISSIVE);

    add_sphere(v3(0.9f, 1.96f, -0.5f), 0.16f, light);

    float R = 50.0f;
    add_sphere(v3(0, -R-1, 0), R, white);    /* floor */
    add_sphere(v3(0, 0, -R-2), R, white);    /* back wall */
    add_sphere(v3(-R-2, 0, 0), R, red);      /* left wall */
    add_sphere(v3(R+2, 0, 0), R, green);     /* right wall */
    add_sphere(v3(0, R+2, 0), R, white);     /* ceiling */

    /* Checkerboard accent on floor */
    for (int ix = -1; ix <= 1; ix++) {
        for (int iz = 0; iz <= 2; iz++) {
            int mat = ((ix + iz) & 1) ? floor_gray : floor_dark;
            add_sphere(v3((float)ix * 0.8f, 0.01f, (float)iz * 0.8f - 0.6f), 0.42f, mat);
        }
    }

    /* ── Nested spheres (glass shell + emissive core) ──
     * Core is placed at the EXACT SAME CENTER as the glass shell.
     * Ray hits glass first → refracts → hits inner core → refracts back out.
     * This creates visible Fresnel on the surface + glowing color inside. */

    /* Front-left: sapphire shell with warm golden core */
    add_sphere(v3(-1.2f, 0.70f, 0.8f), 0.55f, sapphire);
    add_sphere(v3(-1.2f, 0.70f, 0.8f), 0.22f, core_warm);

    /* Front-center: emerald shell with green-glowing core — the showpiece */
    add_sphere(v3(0.0f,  0.75f, 0.5f), 0.65f, emerald);
    add_sphere(v3(0.0f,  0.75f, 0.5f), 0.25f, core_green);

    /* Front-right: amber shell with red-glowing core */
    add_sphere(v3(1.2f,  0.65f, 0.8f), 0.55f, amber);
    add_sphere(v3(1.2f,  0.65f, 0.8f), 0.22f, core_red);

    /* Mid row — standalone showcase spheres */
    add_sphere(v3(1.40f, 0.55f, -0.6f), 0.50f, silver);      /* polished silver */
    add_sphere(v3(0.0f,  0.50f, -1.0f), 0.58f, diamond);     /* diamond (IOR 2.42) */
    add_sphere(v3(-1.2f, 0.40f, -1.3f), 0.48f, copper);      /* copper */

    /* Back row — smooth→rough gradient */
    add_sphere(v3(2.2f, 0.25f, -1.8f), 0.32f, chrome);       /* mirror chrome */
    add_sphere(v3(-2.2f,0.25f, -1.8f), 0.32f, nickel);       /* brushed nickel */
    add_sphere(v3(0.0f, 0.15f, -2.0f), 0.20f, steel);        /* rough steel near wall */

    g_camera.eye = v3(0, 1.3f, 4.0f);
    g_camera.lookat = v3(0, 0.5f, -0.5f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 55.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 4.0f;
}

/* ── Scene 1: Metal Spheres — outdoor ground with reflective variety ── */
static void setup_metal_spheres(void) {
    int ground   = add_material(v3(0.25,0.28,0.35), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int ground2  = add_material(v3(0.55,0.52,0.45), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    /* Metal range: mirror → polished → brushed → rough */
    int mirror   = add_material(v3(0.98,0.96,0.94), v3(0,0,0), 0.0f, 1, MAT_METAL);
    int chrome   = add_material(v3(0.92,0.90,0.87), v3(0,0,0), 0.02f, 1, MAT_METAL);
    int nickel2  = add_material(v3(0.80,0.78,0.72), v3(0,0,0), 0.12f, 1, MAT_METAL);
    int brass    = add_material(v3(0.88,0.68,0.22), v3(0,0,0), 0.08f, 1, MAT_METAL);
    int steel2   = add_material(v3(0.55,0.53,0.50), v3(0,0,0), 0.18f, 1, MAT_METAL);
    int iron     = add_material(v3(0.58,0.54,0.50), v3(0,0,0), 0.28f, 1, MAT_METAL);
    /* Colored glass — deeply saturated for visible tint */
    int glass    = add_material(v3(0.97,0.97,0.97), v3(0,0,0), 0, 1.50f, MAT_DIELECTRIC);
    int ruby     = add_material(v3(0.94,0.06,0.12), v3(0,0,0), 0, 1.54f, MAT_DIELECTRIC);
    int aqua     = add_material(v3(0.08,0.88,0.90), v3(0,0,0), 0, 1.33f, MAT_DIELECTRIC);
    /* Nested: clear glass shell + warm emissive core */
    int core_warm = add_material(v3(1.0,0.85,0.3), v3(3,2,0.3), 0, 1, MAT_EMISSIVE);
    /* Fill lights */
    int fill  = add_material(v3(0.6,0.7,0.9), v3(1.5,2,3), 0, 1, MAT_EMISSIVE);
    int fill2 = add_material(v3(0.9,0.8,0.6), v3(2,1.5,0.8), 0, 1, MAT_EMISSIVE);

    /* Checker ground */
    for (int ix = -3; ix <= 3; ix++) {
        for (int iz = -3; iz <= 3; iz++) {
            int mat = ((ix + iz) & 1) ? ground : ground2;
            add_sphere(v3((float)ix * 2.0f, -0.01f, (float)iz * 2.0f - 1.0f), 1.05f, mat);
        }
    }

    /* Center — large rough-metal anchor */
    add_sphere(v3(0, 0.8f, -1), 1.2f, steel2);

    /* Metal row — full roughness gradient */
    add_sphere(v3(-3.0f, 0.6f, -1.5f), 0.9f, mirror);      /* perfect mirror */
    add_sphere(v3(-1.6f, 0.5f, -2.2f), 0.7f, chrome);      /* polished chrome */
    add_sphere(v3(0.0f,  0.5f, -2.5f), 0.65f, nickel2);    /* brushed nickel */
    add_sphere(v3(1.8f,  0.5f, -2.0f), 0.8f, brass);       /* brushed brass */
    add_sphere(v3(3.0f,  0.5f, -1.2f), 0.9f, iron);        /* rough iron */

    /* Glass row */
    add_sphere(v3(-1.2f, 0.35f, 0.3f), 0.45f, glass);  /* clear glass */
    add_sphere(v3(1.2f,  0.35f, 0.2f), 0.42f, ruby);   /* deep red glass */
    add_sphere(v3(2.5f,  0.30f, -0.5f), 0.38f, aqua);  /* teal glass */

    /* Nested sphere: clear glass shell + warm core — far left */
    add_sphere(v3(-2.5f, 0.50f, -0.3f), 0.48f, glass);
    add_sphere(v3(-2.5f, 0.50f, -0.3f), 0.20f, core_warm);

    /* Distant fill lights */
    add_sphere(v3(-8, 10, 6), 0.8f, fill);
    add_sphere(v3(6, 8, 8), 0.5f, fill2);

    g_camera.eye = v3(0, 1.6f, 4.5f);
    g_camera.lookat = v3(0, 0.5f, -1);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 55.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 4.5f;
}

/* ── Scene 2: Glass & Light — dielectric showcase with caustic lighting ── */
static void setup_glass_light(void) {
    int ground = add_material(v3(0.18,0.18,0.22), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int ground2= add_material(v3(0.35,0.35,0.40), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    /* Dielectrics at different IORs */
    int glass   = add_material(v3(0.98,0.98,0.98), v3(0,0,0), 0, 1.50f, MAT_DIELECTRIC);
    int diamond = add_material(v3(1.0,1.0,1.0), v3(0,0,0), 0, 2.42f, MAT_DIELECTRIC);
    int crystal = add_material(v3(0.90,0.95,1.0), v3(0,0,0), 0, 1.67f, MAT_DIELECTRIC);
    int amber   = add_material(v3(1.0,0.72,0.15), v3(0,0,0), 0, 1.55f, MAT_DIELECTRIC);
    int amethyst= add_material(v3(0.65,0.30,0.85), v3(0,0,0), 0, 1.54f, MAT_DIELECTRIC);
    /* Metals */
    int metal   = add_material(v3(0.95,0.93,0.88), v3(0,0,0), 0.01f, 1, MAT_METAL);
    int gold    = add_material(v3(0.95,0.85,0.30), v3(0,0,0), 0.02f, 1, MAT_METAL);
    /* Colored diffuse accents */
    int red     = add_material(v3(0.85,0.12,0.12), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int blue    = add_material(v3(0.12,0.18,0.88), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int yellow  = add_material(v3(0.90,0.85,0.10), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    /* Lighting — subtle so env maps provide primary illumination */
    int light       = add_material(v3(1,0.95,0.85), v3(0.6,0.5,0.3), 0, 1, MAT_EMISSIVE);
    int warm_light  = add_material(v3(1,0.90,0.7), v3(0.8,0.5,0.2), 0, 1, MAT_EMISSIVE);
    int rim_light   = add_material(v3(0.6,0.7,1.0), v3(0.4,0.6,1.2), 0, 1, MAT_EMISSIVE);
    /* Nested cores */
    int core_warm = add_material(v3(1.0,0.85,0.3), v3(3,2,0.3), 0, 1, MAT_EMISSIVE);
    int core_cool = add_material(v3(0.3,0.5,1.0), v3(0.5,2,5), 0, 1, MAT_EMISSIVE);

    /* Checker ground */
    for (int ix = -3; ix <= 3; ix++) {
        for (int iz = -3; iz <= 3; iz++) {
            int mat = ((ix + iz) & 1) ? ground : ground2;
            add_sphere(v3((float)ix * 1.5f, -0.01f, (float)iz * 1.5f - 1.0f), 0.8f, mat);
        }
    }

    /* Main glass showcase — 5 dielectrics + 2 nested pairs */
    add_sphere(v3(-2.5f, 0.8f, -1.8f), 0.9f, glass);     /* clear glass */
    add_sphere(v3(-1.0f, 0.7f, -3.2f), 0.8f, diamond);   /* high-IOR — spaced clear of glass */
    /* Nested: clear glass shell with warm glow inside */
    add_sphere(v3(0.0f,  0.6f, -1.2f), 0.7f, glass);
    add_sphere(v3(0.0f,  0.6f, -1.2f), 0.25f, core_warm);
    add_sphere(v3(1.5f,  0.5f, -2.0f), 0.7f, amber);     /* warm amber */
    /* Nested: amethyst shell with cool glow */
    add_sphere(v3(2.8f,  0.4f, -1.2f), 0.6f, amethyst);
    add_sphere(v3(2.8f,  0.4f, -1.2f), 0.22f, core_cool);

    /* Crystal accent — back-left corner */
    add_sphere(v3(-0.3f, 0.55f, -3.5f), 0.45f, crystal);

    /* Metal and diffuse accents scattered about */
    add_sphere(v3(-0.6f, 0.3f, 0.1f), 0.35f, metal);     /* polished metal */
    add_sphere(v3(1.0f,  0.25f, 0.3f), 0.3f, gold);      /* gold accent */
    add_sphere(v3(-2.0f, 0.25f, 0.3f), 0.28f, red);      /* red accent */
    add_sphere(v3(0.3f,  0.2f, -3.0f), 0.25f, blue);     /* blue accent */
    add_sphere(v3(-0.8f, 0.2f, -3.2f), 0.22f, yellow);   /* yellow accent */

    /* Subtle light sources — warm overhead, blue rim, warm fill */
    add_sphere(v3(6, 10, 6), 0.3f, light);
    add_sphere(v3(-5, 7, 5), 0.3f, warm_light);
    add_sphere(v3(2, 4, -5), 0.35f, rim_light);

    g_camera.eye = v3(0, 1.5f, 4.0f);
    g_camera.lookat = v3(0, 0.5f, -1.2f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 58.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 4.0f;
}

/* ── Scene 3: Random Spheres — procedural field with rich material mix ── */
static void setup_random_spheres(void) {
    int colors[6];
    colors[0] = add_material(v3(0.95,0.15,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* red */
    colors[1] = add_material(v3(0.15,0.95,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* green */
    colors[2] = add_material(v3(0.15,0.15,0.95), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* blue */
    colors[3] = add_material(v3(0.95,0.95,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* yellow */
    colors[4] = add_material(v3(0.95,0.5,0.1), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);   /* orange */
    colors[5] = add_material(v3(0.75,0.15,0.75), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* purple */
    /* Metals: polished chrome, brushed nickel, gold, rough iron, steel */
    int chrome   = add_material(v3(0.98,0.96,0.93), v3(0,0,0), 0.0f, 1, MAT_METAL);
    int nickel   = add_material(v3(0.80,0.78,0.72), v3(0,0,0), 0.12f, 1, MAT_METAL);
    int gold     = add_material(v3(0.95,0.80,0.25), v3(0,0,0), 0.03f, 1, MAT_METAL);
    int iron     = add_material(v3(0.70,0.65,0.60), v3(0,0,0), 0.25f, 1, MAT_METAL);
    int steel_rnd= add_material(v3(0.55,0.53,0.50), v3(0,0,0), 0.18f, 1, MAT_METAL);
    /* Dielectrics — strong tints */
    int glass    = add_material(v3(0.97,0.97,0.97), v3(0,0,0), 0, 1.50f, MAT_DIELECTRIC);
    int diamond  = add_material(v3(1.0,1.0,1.0), v3(0,0,0), 0, 2.42f, MAT_DIELECTRIC);
    int amber    = add_material(v3(1.0,0.60,0.06), v3(0,0,0), 0, 1.55f, MAT_DIELECTRIC);
    int emerald  = add_material(v3(0.12,0.94,0.25), v3(0,0,0), 0, 1.58f, MAT_DIELECTRIC);
    int ruby_rnd = add_material(v3(0.94,0.06,0.12), v3(0,0,0), 0, 1.54f, MAT_DIELECTRIC);
    int ground_a = add_material(v3(0.25,0.25,0.30), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int ground_b = add_material(v3(0.45,0.45,0.48), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int sun = add_material(v3(1,0.95,0.8), v3(2,1.5,1), 0, 1, MAT_EMISSIVE);

    /* Checker ground */
    for (int ix = -4; ix <= 4; ix++) {
        for (int iz = -4; iz <= 4; iz++) {
            int mat = ((ix + iz) & 1) ? ground_a : ground_b;
            add_sphere(v3((float)ix * 1.5f, -0.01f, (float)iz * 1.5f - 0.5f), 0.8f, mat);
        }
    }

    /* Random field — 40% metal, 30% glass, 30% diffuse */
    unsigned int rng = 42;
    for (int i = 0; i < 55; i++) {
        float x = (randf(&rng) - 0.5f) * 8.0f;
        float z = (randf(&rng) - 0.5f) * 7.0f - 1.0f;
        float r = 0.12f + randf(&rng) * 0.40f;
        float y = r;
        int mat;
        float choice = randf(&rng);
        if (choice < 0.08f)      mat = chrome;
        else if (choice < 0.15f) mat = nickel;
        else if (choice < 0.20f) mat = gold;
        else if (choice < 0.27f) mat = iron;
        else if (choice < 0.33f) mat = steel_rnd;
        else if (choice < 0.40f) mat = glass;
        else if (choice < 0.45f) mat = diamond;
        else if (choice < 0.51f) mat = amber;
        else if (choice < 0.56f) mat = emerald;
        else if (choice < 0.60f) mat = ruby_rnd;
        else                     mat = colors[(int)(randf(&rng) * 6)];
        add_sphere(v3(x, y, z), r, mat);
    }

    /* Large center diamond — sparkle showcase */
    add_sphere(v3(0, 0.9f, -1.6f), 0.85f, diamond);

    /* Sun far above */
    add_sphere(v3(8, 16, 12), 0.3f, sun);

    g_camera.eye = v3(0, 2.2f, 5.0f);
    g_camera.lookat = v3(0, 0.5f, -0.5f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 62.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 5.0f;
}

/* ── Scene 4: Checkerboard — patterned floor with reflective and refractive columns ── */
static void setup_checkerboard(void) {
    int white = add_material(v3(0.85,0.85,0.85), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int black = add_material(v3(0.12,0.12,0.12), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    /* Columns — mix of metals and dielectrics */
    int red_metal  = add_material(v3(0.85,0.15,0.15), v3(0,0,0), 0.03f, 1, MAT_METAL);
    int blue_metal = add_material(v3(0.15,0.20,0.85), v3(0,0,0), 0.06f, 1, MAT_METAL);
    int gold_metal = add_material(v3(0.95,0.82,0.25), v3(0,0,0), 0.02f, 1, MAT_METAL);
    int glass_col  = add_material(v3(0.95,0.95,0.95), v3(0,0,0), 0, 1.52f, MAT_DIELECTRIC);
    int amber_col  = add_material(v3(1.0,0.70,0.12), v3(0,0,0), 0, 1.55f, MAT_DIELECTRIC);
    /* Centerpiece spheres */
    int chrome  = add_material(v3(0.97,0.95,0.92), v3(0,0,0), 0.0f, 1, MAT_METAL);
    int diamond = add_material(v3(1.0,1.0,1.0), v3(0,0,0), 0, 2.42f, MAT_DIELECTRIC);
    int ruby    = add_material(v3(0.94,0.06,0.12), v3(0,0,0), 0, 1.54f, MAT_DIELECTRIC);
    int emerald = add_material(v3(0.10,0.94,0.22), v3(0,0,0), 0, 1.58f, MAT_DIELECTRIC);
    int sapphire = add_material(v3(0.25,0.40,1.0), v3(0,0,0), 0, 1.77f, MAT_DIELECTRIC);
    int sun   = add_material(v3(1,1,1), v3(3,2.5,2), 0, 1, MAT_EMISSIVE);

    /* Checkerboard floor */
    for (int ix = -7; ix <= 7; ix++) {
        for (int iz = -6; iz <= 6; iz++) {
            int mat = ((ix + iz) & 1) ? white : black;
            add_sphere(v3((float)ix * 1.5f, -0.70f, (float)iz * 1.5f - 2.0f), 0.95f, mat);
        }
    }

    /* Columns — each column alternating metal/dielectric */
    /* Left column: red metal */
    add_sphere(v3(-5.0f, 1.2f, -3.0f), 0.50f, red_metal);
    add_sphere(v3(-5.0f, 2.6f, -3.0f), 0.45f, red_metal);
    add_sphere(v3(-5.0f, 4.0f, -3.0f), 0.40f, red_metal);
    /* Mid-left column: glass */
    add_sphere(v3(-2.5f, 1.3f, -1.5f), 0.50f, glass_col);
    add_sphere(v3(-2.5f, 2.7f, -1.5f), 0.45f, glass_col);
    add_sphere(v3(-2.5f, 4.1f, -1.5f), 0.40f, glass_col);
    /* Center column: blue metal */
    add_sphere(v3(0.0f,  1.2f, -2.5f), 0.52f, blue_metal);
    add_sphere(v3(0.0f,  2.6f, -2.5f), 0.48f, blue_metal);
    add_sphere(v3(0.0f,  4.0f, -2.5f), 0.42f, blue_metal);
    /* Mid-right column: amber glass */
    add_sphere(v3(2.5f,  1.3f, -1.5f), 0.50f, amber_col);
    add_sphere(v3(2.5f,  2.7f, -1.5f), 0.45f, amber_col);
    add_sphere(v3(2.5f,  4.1f, -1.5f), 0.40f, amber_col);
    /* Right column: gold metal */
    add_sphere(v3(5.0f,  1.2f, -3.0f), 0.50f, gold_metal);
    add_sphere(v3(5.0f,  2.6f, -3.0f), 0.45f, gold_metal);
    add_sphere(v3(5.0f,  4.0f, -3.0f), 0.40f, gold_metal);

    /* Centerpiece showcase — 4 gemstones in a row */
    add_sphere(v3(-1.8f, 0.5f, -2.5f), 0.48f, ruby);
    add_sphere(v3(-0.6f, 0.55f, -2.5f), 0.50f, diamond);
    add_sphere(v3(0.6f,  0.5f, -2.5f), 0.48f, emerald);
    add_sphere(v3(1.8f,  0.45f, -2.5f), 0.48f, sapphire);
    /* Chrome sphere in front */
    add_sphere(v3(0.0f, 0.35f, -0.8f), 0.55f, chrome);

    /* Distant sun */
    add_sphere(v3(10, 18, 12), 0.5f, sun);

    g_camera.eye = v3(0, 2.0f, 5.5f);
    g_camera.lookat = v3(0, 1.2f, -2.0f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 58.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 5.5f;
}

/* ── Scene 5: Cosmic — floating gemstones in deep space ── */
static void setup_cosmic(void) {
    /* ── Deep-space backdrop ──
     * Centered at origin, R=100 encloses camera and all scene objects.
     * Every ray that misses scene geometry hits this self-luminous dome.
     * Kept nearly black — stars and moons are the only real light sources. */
    int void_mat   = add_material(v3(0,0,0), v3(0.003,0.002,0.006), 0, 1, MAT_EMISSIVE);
    add_sphere(v3(0, 0, 0), 100.0f, void_mat);
    g_cosmic_backdrop_idx = g_num_spheres - 1;  /* track for HDRI skip */

    /* Glass orbs — lighter albeo so they reflect/refract the night glow */
    int orb_ice    = add_material(v3(0.55,0.65,0.90), v3(0,0,0), 0, 1.31f, MAT_DIELECTRIC);
    int orb_coral  = add_material(v3(0.80,0.25,0.35), v3(0,0,0), 0, 1.40f, MAT_DIELECTRIC);
    int orb_emerald= add_material(v3(0.18,0.70,0.28), v3(0,0,0), 0, 1.58f, MAT_DIELECTRIC);
    int orb_amber  = add_material(v3(0.75,0.42,0.12), v3(0,0,0), 0, 1.55f, MAT_DIELECTRIC);
    int orb_ameth  = add_material(v3(0.55,0.22,0.75), v3(0,0,0), 0, 1.54f, MAT_DIELECTRIC);
    int orb_diamond= add_material(v3(0.80,0.80,0.80), v3(0,0,0), 0, 2.42f, MAT_DIELECTRIC);
    /* Rough metals — lighter so they catch what little light exists */
    int obsidian   = add_material(v3(0.18,0.14,0.30), v3(0,0,0), 0.06f, 1, MAT_METAL);
    int dark_iron  = add_material(v3(0.25,0.25,0.30), v3(0,0,0), 0.30f, 1, MAT_METAL);

    /* Stars — bright enough to be hit by recursive rays and light the scene */
    int star_warm  = add_material(v3(1.0,0.85,0.6), v3(8,3,0.6), 0, 1, MAT_EMISSIVE);
    int star_cool  = add_material(v3(0.7,0.8,1.0), v3(1.5,4,12), 0, 1, MAT_EMISSIVE);
    int star_red   = add_material(v3(1.0,0.3,0.2), v3(6,0.5,0.15), 0, 1, MAT_EMISSIVE);
    int star_blue  = add_material(v3(0.2,0.4,1.0), v3(0.5,1,10), 0, 1, MAT_EMISSIVE);

    /* Distant nebula — faint haze */
    int nebula     = add_material(v3(0.06,0.03,0.14), v3(0.02,0.01,0.06), 0, 1, MAT_EMISSIVE);
    int nebula2    = add_material(v3(0.03,0.06,0.04), v3(0.01,0.03,0.01), 0, 1, MAT_EMISSIVE);

    /* Ground — dark asteroid surface */
    int ground     = add_material(v3(0.015,0.015,0.025), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int ground2    = add_material(v3(0.025,0.025,0.040), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);

    /* Nested cores — brighter inner glow */
    int core_green = add_material(v3(0.08,0.45,0.12), v3(0.15,2,0.3), 0, 1, MAT_EMISSIVE);
    int core_warm  = add_material(v3(0.45,0.30,0.08), v3(2,0.8,0.08), 0, 1, MAT_EMISSIVE);

    /* Moons — bright celestial bodies that light the scene */
    int moon       = add_material(v3(0.92,0.89,0.82), v3(12,10,7), 0, 1, MAT_EMISSIVE);
    int moon2      = add_material(v3(0.75,0.78,0.92), v3(5,7,12), 0, 1, MAT_EMISSIVE);
    add_sphere(v3(6, 5, -22), 2.0f, moon);
    add_sphere(v3(-5, 3, -20), 1.5f, moon2);

    /* Stars — all placed IN FRONT of backdrop (z > -6) */
    unsigned int rng = 137;
    int star_mats[] = { star_warm, star_cool, star_red, star_blue, star_cool, star_warm };
    for (int i = 0; i < 250; i++) {
        float theta = 2.0f * PI * randf(&rng);
        /* Phi restricted so stars are above ground and in front of backdrop */
        float phi = acosf(2.0f * randf(&rng) - 1.0f);
        float sr = 6.0f + randf(&rng) * 16.0f;
        float sx = sr * sinf(phi) * cosf(theta);
        float sy = sr * sinf(phi) * sinf(theta);
        float sz = sr * cosf(phi) - 1.0f;
        /* Discard: below ground or behind backdrop front surface (z < -6) */
        if (sy < -1.0f || sz < -6.0f) continue;
        float size = 0.06f + randf(&rng) * 0.20f;
        int smat = star_mats[(int)(randf(&rng) * 6)];
        add_sphere(v3(sx, sy, sz), size, smat);
    }

    /* Nebula clouds — few, faint, in front of backdrop */
    for (int i = 0; i < 5; i++) {
        float nx = (randf(&rng) - 0.5f) * 16.0f;
        float ny = (randf(&rng) - 0.5f) * 8.0f + 2.0f;
        float nz = -7.0f + (randf(&rng) - 0.5f) * 12.0f;
        if (nz < -6.0f) nz = -5.0f;
        int nm = (i & 1) ? nebula : nebula2;
        add_sphere(v3(nx, ny, nz), 0.8f + randf(&rng) * 2.0f, nm);
    }

    /* Ground disc — dark checker */
    for (int ix = -2; ix <= 2; ix++) {
        for (int iz = -2; iz <= 2; iz++) {
            int mat = ((ix + iz) & 1) ? ground : ground2;
            add_sphere(v3((float)ix * 2.0f, -3.8f, (float)iz * 1.8f - 1.0f), 1.2f, mat);
        }
    }

    /* Orb formation — colored glass, rough metal accents */
    add_sphere(v3(-2.2f, 0.8f, -2.2f), 0.9f, orb_ice);     /* top-left: ice glass */
    add_sphere(v3(2.0f,  0.5f, -1.8f), 0.75f, orb_coral);  /* top-right: coral */
    /* Nested: emerald shell + green core */
    add_sphere(v3(0.5f,  0.0f, -3.0f), 0.65f, orb_emerald);
    add_sphere(v3(0.5f,  0.0f, -3.0f), 0.22f, core_green);
    add_sphere(v3(-1.6f, 0.3f, -3.8f), 0.45f, obsidian);   /* mid-left: dark shiny */
    add_sphere(v3(2.2f,  0.1f, -3.2f), 0.50f, orb_amber);  /* mid-right: amber */
    add_sphere(v3(0.8f,  0.1f, -2.8f), 0.38f, dark_iron);  /* rough dark metal */
    /* Nested: diamond shell + warm core */
    add_sphere(v3(0.0f, -0.4f, -1.6f), 0.40f, orb_diamond);
    add_sphere(v3(0.0f, -0.4f, -1.6f), 0.14f, core_warm);
    /* Bonus floating small orbs */
    add_sphere(v3(-0.8f, -0.6f, -2.0f), 0.30f, orb_ameth);
    add_sphere(v3(1.4f,  -0.3f, -2.4f), 0.28f, obsidian);

    g_camera.eye = v3(0, 0.1f, 3.2f);
    g_camera.lookat = v3(0, 0.1f, -2.0f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 65.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 4.5f;
}

/* ── Ray-sphere intersection ── */
static int intersect(Ray *ray, Hit *hit) {
    hit->t = INF;
    hit->mat_id = -1;

    for (int i = 0; i < g_num_spheres; i++) {
        /* Skip Cosmic backdrop when HDRI env map is active — HDRI provides the background */
        if (g_use_env_map && i == g_cosmic_backdrop_idx) continue;
        Sphere *s = &g_spheres[i];
        Vec3 oc = v3_sub(ray->origin, s->center);
        float a = v3_dot(ray->dir, ray->dir);
        float half_b = v3_dot(oc, ray->dir);
        float c = v3_dot(oc, oc) - s->radius * s->radius;
        float disc = half_b * half_b - a * c;

        if (disc < 0) continue;

        float sqrt_disc = sqrtf(disc);
        float root = (-half_b - sqrt_disc) / a;
        if (root < 0.001f || root >= hit->t)
            root = (-half_b + sqrt_disc) / a;
        if (root < 0.001f || root >= hit->t)
            continue;

        hit->t = root;
        hit->point = v3_add(ray->origin, v3_mul(ray->dir, root));
        Vec3 outward = v3_mul(v3_sub(hit->point, s->center), 1.0f / s->radius);
        hit->front_face = v3_dot(ray->dir, outward) < 0;
        hit->normal = hit->front_face ? outward : v3_mul(outward, -1.0f);
        hit->mat_id = s->mat_id;
    }
    return hit->t < INF;
}

/* ── Environment map sampling (equirectangular with bilinear interpolation) ── */
static Vec3 env_map_sample(Vec3 dir) {
    if (!g_env_map || g_env_w == 0 || g_env_h == 0)
        return v3(0, 0, 0);

    /* Equirectangular mapping: azimuth + elevation → UV */
    float phi = atan2f(dir.z, dir.x);          /* [-PI, PI] */
    float theta = asinf(clampf(dir.y, -1.0f, 1.0f)); /* [-PI/2, PI/2] */

    float u = 0.5f + phi / (2.0f * PI);        /* [0, 1] */
    float v = 0.5f - theta / PI;               /* [0, 1] — flip: v=0 is top (zenith) */

    /* Bilinear interpolation */
    float px = u * (float)g_env_w - 0.5f;
    float py = v * (float)g_env_h - 0.5f;

    int x0 = (int)floorf(px);
    int y0 = (int)floorf(py);
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    /* Wrap horizontally */
    x0 = (x0 % g_env_w + g_env_w) % g_env_w;
    x1 = (x1 % g_env_w + g_env_w) % g_env_w;

    /* Clamp vertically */
    if (y0 < 0) y0 = 0;
    if (y1 >= g_env_h) y1 = g_env_h - 1;
    if (y0 >= g_env_h) y0 = g_env_h - 1;

    float fx = px - floorf(px);
    float fy = py - floorf(py);

    float *p00 = g_env_map + (y0 * g_env_w + x0) * 3;
    float *p10 = g_env_map + (y0 * g_env_w + x1) * 3;
    float *p01 = g_env_map + (y1 * g_env_w + x0) * 3;
    float *p11 = g_env_map + (y1 * g_env_w + x1) * 3;

    /* Pre-normalized HDRI values (99.9th percentile ≈ 4.0, clamped at ~12).
     * Safety clamp: cap any residual outliers at 15 to prevent ACES bleaching. */
    float r = ((p00[0] * (1.0f - fx) + p10[0] * fx) * (1.0f - fy) +
               (p01[0] * (1.0f - fx) + p11[0] * fx) * fy);
    float g = ((p00[1] * (1.0f - fx) + p10[1] * fx) * (1.0f - fy) +
               (p01[1] * (1.0f - fx) + p11[1] * fx) * fy);
    float b = ((p00[2] * (1.0f - fx) + p10[2] * fx) * (1.0f - fy) +
               (p01[2] * (1.0f - fx) + p11[2] * fx) * fy);

    return v3(clampf(r, 0.0f, 15.0f),
              clampf(g, 0.0f, 15.0f),
              clampf(b, 0.0f, 15.0f));
}

/* ── Procedural noise for clouds ── */
static float hash_float(int ix, int iy) {
    int n = (ix * 1271 + iy * 3117) ^ (ix * 3769 + iy * 6133);
    n = (n << 13) ^ n;
    return (float)((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 2147483648.0f;
}

static float smooth_noise(float u, float v, float scale) {
    float su = u * scale;
    float sv = v * scale;
    int iu = (int)floorf(su);
    int iv = (int)floorf(sv);
    float fu = su - (float)iu;
    float fv = sv - (float)iv;
    /* Smoothstep */
    fu = fu * fu * (3.0f - 2.0f * fu);
    fv = fv * fv * (3.0f - 2.0f * fv);
    float n00 = hash_float(iu, iv);
    float n10 = hash_float(iu + 1, iv);
    float n01 = hash_float(iu, iv + 1);
    float n11 = hash_float(iu + 1, iv + 1);
    return n00 * (1.0f - fu) * (1.0f - fv) +
           n10 * fu * (1.0f - fv) +
           n01 * (1.0f - fu) * fv +
           n11 * fu * fv;
}

static float fbm(Vec3 dir, int octaves, float lacunarity, float gain) {
    /* Map direction to a sphere-unwrapped UV using azimuth and elevation */
    float az = atan2f(dir.z, dir.x);          /* [-PI, PI] */
    float el = asinf(clampf(dir.y, -1.0f, 1.0f)); /* [-PI/2, PI/2] */
    float u = az / (2.0f * PI) + 0.5f;        /* [0, 1] */
    float v = el / PI + 0.5f;                 /* [0, 1] */

    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_val = 0.0f;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * smooth_noise(u, v, frequency * 4.0f);
        max_val += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    return value / max_val;
}

/* ── Rich procedural sky: atmospheric scattering, clouds, sun, distant lights ── */
static Vec3 sky_color(Ray *ray) {
    Vec3 dir = v3_norm(ray->dir);
    float t = 0.5f * (dir.y + 1.0f);  /* 0 = nadir, 1 = zenith */

    /* ── Atmospheric scattering gradient ──
     * Ground: warm brown/tan → near-horizon: golden → mid-sky: blue → zenith: deep blue */
    Vec3 ground_color  = v3(0.35f, 0.25f, 0.18f);
    Vec3 glow_color    = v3(0.95f, 0.72f, 0.38f);  /* golden horizon glow */
    Vec3 sky_mid       = v3(0.38f, 0.55f, 0.92f);
    Vec3 zenith_color  = v3(0.15f, 0.25f, 0.72f);

    Vec3 sky;
    if (t < 0.22f) {
        sky = v3_lerp(ground_color, glow_color, t / 0.22f);
    } else if (t < 0.58f) {
        sky = v3_lerp(glow_color, sky_mid, (t - 0.22f) / 0.36f);
    } else {
        sky = v3_lerp(sky_mid, zenith_color, (t - 0.58f) / 0.42f);
    }

    /* ── Procedural clouds (FBM noise) ── */
    float cloud_noise = fbm(dir, 5, 2.3f, 0.55f);
    /* Remap: flatten the bottom, sharpen the top */
    float cloud = (cloud_noise - 0.38f) * 3.0f;
    cloud = clampf(cloud, 0.0f, 1.0f);
    /* More clouds at mid-altitudes, fewer overhead */
    float cloud_mask = 1.0f - fabsf(t - 0.45f) * 2.5f;
    cloud_mask = clampf(cloud_mask, 0.1f, 1.0f);
    cloud *= cloud_mask;

    /* Cloud lighting: brighter where sun would hit them, darker underneath */
    Vec3 sun_dir = v3_norm(v3(0.50f, 0.58f, 0.48f));
    float sun_facing = v3_dot(dir, sun_dir);
    Vec3 cloud_light = v3(0.95f, 0.92f, 0.85f);
    Vec3 cloud_dark  = v3(0.60f, 0.56f, 0.52f);
    Vec3 cloud_color = v3_lerp(cloud_dark, cloud_light, clampf(sun_facing * 0.7f + 0.5f, 0.0f, 1.0f));
    sky = v3_lerp(sky, cloud_color, cloud * 0.55f);

    /* ── Sun disk and glow ── */
    float sun_dot = v3_dot(dir, sun_dir);

    /* Wide atmospheric glow around sun (Mie scattering) */
    if (sun_dot > 0.92f) {
        float g = (sun_dot - 0.92f) / 0.08f;
        g = g * g * (3.0f - 2.0f * g);  /* smoothstep */
        Vec3 mie_glow = v3(1.6f, 1.2f, 0.75f);
        sky = v3_lerp(sky, mie_glow, g * 0.55f);
    }

    /* Tight glow ring */
    if (sun_dot > 0.97f) {
        float g = (sun_dot - 0.97f) / 0.03f;
        g = g * g * (3.0f - 2.0f * g);  /* smoothstep */
        Vec3 tight_glow = v3(2.2f, 1.7f, 1.1f);
        sky = v3_lerp(sky, tight_glow, g * 0.5f);
    }

    /* Sun disk */
    if (sun_dot > 0.9993f) {
        float s = (sun_dot - 0.9993f) / 0.0007f;
        s = clampf(s, 0.0f, 1.0f);
        Vec3 sun = v3(6.0f, 4.5f, 3.0f);
        sky = v3_lerp(sky, sun, s * s);
    }

    /* ── Secondary colored lights for interesting reflections ── */
    Vec3 light1_dir = v3_norm(v3(-0.40f, 0.30f, -0.25f));
    Vec3 light2_dir = v3_norm(v3(0.60f, 0.15f, -0.35f));
    float l1_dot = v3_dot(dir, light1_dir);
    float l2_dot = v3_dot(dir, light2_dir);

    /* Warm orange point (like a distant sunset reflection) */
    if (l1_dot > 0.92f) {
        float g = (l1_dot - 0.92f) / 0.08f;
        g = g * g * (3.0f - 2.0f * g);
        Vec3 warm_light = v3(1.2f, 0.6f, 0.22f);
        sky = v3_lerp(sky, warm_light, g * 0.22f);
    }

    /* Cool blue point */
    if (l2_dot > 0.90f) {
        float g = (l2_dot - 0.90f) / 0.10f;
        g = g * g * (3.0f - 2.0f * g);
        Vec3 cool_light = v3(0.2f, 0.4f, 1.0f);
        sky = v3_lerp(sky, cool_light, g * 0.18f);
    }

    /* ── Keep sky values non-negative ── */
    sky.x = maxf(sky.x, 0.0f);
    sky.y = maxf(sky.y, 0.0f);
    sky.z = maxf(sky.z, 0.0f);

    return sky;
}

/* ── Cosine-weighted hemisphere sampling ── */
static Vec3 random_hemisphere(Vec3 normal, unsigned int *seed) {
    float r1 = randf(seed);
    float r2 = randf(seed);
    float phi = 2.0f * PI * r1;
    float cos_theta = sqrtf(1.0f - r2);
    float sin_theta = sqrtf(r2);

    Vec3 w = normal;
    Vec3 u, v;
    if (fabsf(w.x) > 0.1f) {
        u = v3_norm(v3_cross(v3(0,1,0), w));
    } else {
        u = v3_norm(v3_cross(v3(1,0,0), w));
    }
    v = v3_cross(w, u);

    return v3_norm(v3_add(
        v3_add(v3_mul(u, cos_theta * cosf(phi)), v3_mul(v, cos_theta * sinf(phi))),
        v3_mul(w, sin_theta)
    ));
}

/* ── Fresnel (Schlick) ── */
static float schlick(float cosine, float ior) {
    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 = r0 * r0;
    return r0 + (1.0f - r0) * powf(1.0f - cosine, 5.0f);
}

/* ── ACES Filmic Tone Mapping ── */
static inline float aces_tonemap(float x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    float v = (x * (a * x + b)) / (x * (c * x + d) + e);
    return clampf(v, 0.0f, 1.0f);
}

/* ── Path tracing ── */
static Vec3 trace(Ray *ray, unsigned int *seed, int depth) {
    if (depth >= MAX_DEPTH) return v3(0,0,0);

    Hit hit;
    if (!intersect(ray, &hit)) {
        return g_use_env_map ? env_map_sample(ray->dir) : sky_color(ray);
    }

    Material *mat = &g_materials[hit.mat_id];

    if (mat->type == MAT_EMISSIVE) {
        return v3(maxf(mat->emission.x, 0.0f),
                  maxf(mat->emission.y, 0.0f),
                  maxf(mat->emission.z, 0.0f));
    }

    if (depth > RR_DEPTH) {
        float p = maxf(maxf(mat->albedo.x, mat->albedo.y), mat->albedo.z);
        if (randf(seed) > p) return v3(0,0,0);
    }

    Vec3 wo = v3_mul(ray->dir, -1.0f);

    switch (mat->type) {
    case MAT_LAMBERTIAN: {
        Vec3 wi = random_hemisphere(hit.normal, seed);
        float cos_theta = v3_dot(wi, hit.normal);
        if (cos_theta <= 0) return v3(0,0,0);

        Ray scattered;
        scattered.origin = hit.point;
        scattered.dir = wi;

        Vec3 brdf = v3_mul(mat->albedo, 1.0f / PI);
        Vec3 Li = trace(&scattered, seed, depth + 1);
        return v3_mul(v3_mulv(brdf, Li), cos_theta * 2.0f * PI);
    }
    case MAT_METAL: {
        Vec3 reflected = v3_sub(wo, v3_mul(hit.normal, 2.0f * v3_dot(wo, hit.normal)));
        if (mat->roughness > 0.001f) {
            Vec3 jitter = random_hemisphere(reflected, seed);
            reflected = v3_norm(v3_lerp(reflected, jitter, mat->roughness));
        }

        Ray scattered;
        scattered.origin = hit.point;
        scattered.dir = reflected;

        if (v3_dot(reflected, hit.normal) <= 0) return v3(0,0,0);
        Vec3 Li = trace(&scattered, seed, depth + 1);
        return v3_mulv(mat->albedo, Li);
    }
    case MAT_DIELECTRIC: {
        float ior = hit.front_face ? (1.0f / mat->ior) : mat->ior;
        float cos_theta = fminf(v3_dot(wo, hit.normal), 1.0f);
        float sin_theta = sqrtf(1.0f - cos_theta * cos_theta);
        int cannot_refract = ior * sin_theta > 1.0f;

        Vec3 direction;
        if (cannot_refract || schlick(cos_theta, ior) > randf(seed)) {
            direction = v3_sub(wo, v3_mul(hit.normal, 2.0f * cos_theta));
        } else {
            Vec3 r_out_perp = v3_mul(v3_add(wo, v3_mul(hit.normal, cos_theta)), ior);
            Vec3 r_out_parallel = v3_mul(hit.normal, -sqrtf(fabsf(1.0f - v3_dot(r_out_perp, r_out_perp))));
            direction = v3_add(r_out_perp, r_out_parallel);
        }

        Ray scattered;
        scattered.origin = hit.point;
        scattered.dir = direction;

        Vec3 Li = trace(&scattered, seed, depth + 1);
        return v3_mulv(mat->albedo, Li);
    }
    default:
        return v3(0,0,0);
    }
}

/* ── Camera ray generation ── */
static Ray get_ray(float u, float v, unsigned int *seed) {
    float theta = g_camera.fov * PI / 180.0f;
    float half_h = tanf(theta * 0.5f);
    float half_w = half_h * (float)g_width / (float)g_height;

    Vec3 w = v3_norm(v3_sub(g_camera.eye, g_camera.lookat));
    Vec3 uu = v3_norm(v3_cross(g_camera.up, w));
    Vec3 vv = v3_cross(w, uu);

    Vec3 lower_left = v3_sub(v3_sub(v3_sub(g_camera.eye,
        v3_mul(uu, half_w * g_camera.focus_dist)),
        v3_mul(vv, half_h * g_camera.focus_dist)),
        v3_mul(w, g_camera.focus_dist));

    Vec3 horizontal = v3_mul(uu, 2.0f * half_w * g_camera.focus_dist);
    Vec3 vertical = v3_mul(vv, 2.0f * half_h * g_camera.focus_dist);

    Vec3 lens_radius = v3(0,0,0);
    Vec3 origin = g_camera.eye;
    if (g_camera.aperture > 0.001f) {
        float r1 = randf(seed);
        float r2 = randf(seed);
        float lens_r = g_camera.aperture * 0.5f;
        float theta_lens = 2.0f * PI * r1;
        float r = lens_r * sqrtf(r2);
        lens_radius = v3_add(v3_mul(uu, r * cosf(theta_lens)), v3_mul(vv, r * sinf(theta_lens)));
        origin = v3_add(g_camera.eye, lens_radius);
    }

    Vec3 target = v3_add(v3_add(lower_left, v3_mul(horizontal, u)), v3_mul(vertical, v));
    Ray ray;
    ray.origin = origin;
    ray.dir = v3_norm(v3_sub(target, origin));
    return ray;
}

/* ── Public API ── */

EMSCRIPTEN_KEEPALIVE
void init(int width, int height, int scene_id) {
    free_scene();
    g_width = width;
    g_height = height;
    g_scene_id = scene_id;
    g_total_samples = 0;

    int pixel_count = width * height;
    if (g_accum) free(g_accum);
    g_accum = (float*)calloc(pixel_count * 4, sizeof(float));
    if (g_output) free(g_output);
    g_output = (unsigned char*)malloc(pixel_count * 4);

    switch (scene_id) {
        case 0: setup_cornell_box(); break;
        case 1: setup_metal_spheres(); break;
        case 2: setup_glass_light(); break;
        case 3: setup_random_spheres(); break;
        case 4: setup_checkerboard(); break;
        case 5: setup_cosmic(); break;
        default: setup_cornell_box(); break;
    }
}

EMSCRIPTEN_KEEPALIVE
void render(int samples_per_pixel) {
    if (!g_accum || !g_output) return;

    for (int y = 0; y < g_height; y++) {
        for (int x = 0; x < g_width; x++) {
            int idx = (y * g_width + x) * 4;
            Vec3 color = v3(0,0,0);

            for (int s = 0; s < samples_per_pixel; s++) {
                unsigned int seed = (g_total_samples + s) * g_width * g_height +
                                    y * g_width + x + 1;
                float u = ((float)x + randf(&seed)) / (float)g_width;
                /* FIX: flip v so y=0 maps to top of viewport (matches Canvas2D) */
                float v = 1.0f - ((float)y + randf(&seed)) / (float)g_height;
                Ray ray = get_ray(u, v, &seed);
                Vec3 c = trace(&ray, &seed, 0);
                color = v3_add(color, c);
            }

            /* Clamp extreme values to prevent NaN/inf propagation */
            color.x = clampf(color.x, 0.0f, 1000.0f);
            color.y = clampf(color.y, 0.0f, 1000.0f);
            color.z = clampf(color.z, 0.0f, 1000.0f);

            color = v3_mul(color, 1.0f / (float)samples_per_pixel);

            float old_r = g_accum[idx];
            float old_g = g_accum[idx + 1];
            float old_b = g_accum[idx + 2];
            float count = g_accum[idx + 3];

            float new_count = count + (float)samples_per_pixel;
            g_accum[idx]     = (old_r * count + color.x * samples_per_pixel) / new_count;
            g_accum[idx + 1] = (old_g * count + color.y * samples_per_pixel) / new_count;
            g_accum[idx + 2] = (old_b * count + color.z * samples_per_pixel) / new_count;
            g_accum[idx + 3] = new_count;

            float r = g_accum[idx];
            float g = g_accum[idx + 1];
            float b = g_accum[idx + 2];

            r = aces_tonemap(r);
            g = aces_tonemap(g);
            b = aces_tonemap(b);

            r = powf(r, 1.0f/2.2f);
            g = powf(g, 1.0f/2.2f);
            b = powf(b, 1.0f/2.2f);

            g_output[idx]     = (unsigned char)(r * 255);
            g_output[idx + 1] = (unsigned char)(g * 255);
            g_output[idx + 2] = (unsigned char)(b * 255);
            g_output[idx + 3] = 255;
        }
    }
    g_total_samples += samples_per_pixel;
}

EMSCRIPTEN_KEEPALIVE
unsigned char* get_pixels(void) {
    return g_output;
}

EMSCRIPTEN_KEEPALIVE
int get_total_samples(void) {
    return g_total_samples;
}

EMSCRIPTEN_KEEPALIVE
void set_camera(float ex, float ey, float ez,
                float lx, float ly, float lz) {
    g_camera.eye = v3(ex, ey, ez);
    g_camera.lookat = v3(lx, ly, lz);
    g_total_samples = 0;
    if (g_accum) memset(g_accum, 0, g_width * g_height * 4 * sizeof(float));
}

/* Convenience: orbit camera around lookat point */
EMSCRIPTEN_KEEPALIVE
void look_at(float distance, float yaw, float pitch) {
    float yaw_rad = yaw * PI / 180.0f;
    float pitch_rad = pitch * PI / 180.0f;

    float ex = g_camera.lookat.x + distance * cosf(pitch_rad) * sinf(yaw_rad);
    float ey = g_camera.lookat.y + distance * sinf(pitch_rad);
    float ez = g_camera.lookat.z + distance * cosf(pitch_rad) * cosf(yaw_rad);

    set_camera(ex, ey, ez, g_camera.lookat.x, g_camera.lookat.y, g_camera.lookat.z);
}

/* ── Environment map API ── */

EMSCRIPTEN_KEEPALIVE
void load_env_map(float *data, int w, int h) {
    if (g_env_map) { free(g_env_map); g_env_map = NULL; }
    g_env_w = w;
    g_env_h = h;
    int count = w * h * 3;
    g_env_map = (float*)malloc(count * sizeof(float));
    if (g_env_map && data) {
        memcpy(g_env_map, data, count * sizeof(float));
    }
}

EMSCRIPTEN_KEEPALIVE
void set_use_env_map(int use) {
    g_use_env_map = use;
    g_total_samples = 0;
    if (g_accum) memset(g_accum, 0, g_width * g_height * 4 * sizeof(float));
}

EMSCRIPTEN_KEEPALIVE
int get_env_map_active(void) {
    return g_use_env_map && g_env_map != NULL;
}

EMSCRIPTEN_KEEPALIVE
void destroy(void) {
    free_scene();
    if (g_accum) { free(g_accum); g_accum = NULL; }
    if (g_output) { free(g_output); g_output = NULL; }
    if (g_env_map) { free(g_env_map); g_env_map = NULL; }
    g_env_w = g_env_h = 0;
    g_width = g_height = 0;
}
