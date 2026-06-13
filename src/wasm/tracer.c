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
static int g_num_materials = 0;
static Camera g_camera;
static int g_scene_id = 0;
static int g_total_samples = 0;

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

/* ── Scene 0: Cornell Box ── */
static void setup_cornell_box(void) {
    int white = add_material(v3(0.95,0.93,0.88), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int red   = add_material(v3(0.75,0.08,0.08), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int green = add_material(v3(0.08,0.55,0.12), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int floor_gray = add_material(v3(0.4,0.4,0.45), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int floor_dark = add_material(v3(0.15,0.15,0.18), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int light = add_material(v3(1,1,1), v3(15,15,15), 0, 1, MAT_EMISSIVE);
    int metal = add_material(v3(0.92,0.88,0.82), v3(0,0,0), 0.05f, 1, MAT_METAL);
    int glass = add_material(v3(0.98,0.98,0.98), v3(0,0,0), 0, 1.5f, MAT_DIELECTRIC);

    add_sphere(v3(1.0f, 1.95f, -0.5f), 0.18f, light);

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

    add_sphere(v3(1.35f, 0.3f, -0.8f), 0.7f, metal);
    add_sphere(v3(-0.4f, 0.2f, -1.0f), 0.6f, glass);

    g_camera.eye = v3(0, 1.3f, 4.0f);
    g_camera.lookat = v3(0, 0.5f, -0.5f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 50.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 4.0f;
}

/* ── Scene 1: Metal Spheres ── */
static void setup_metal_spheres(void) {
    int ground   = add_material(v3(0.25,0.28,0.35), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int ground2  = add_material(v3(0.55,0.52,0.45), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int center   = add_material(v3(0.8,0.25,0.25), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int left     = add_material(v3(0.85,0.85,0.85), v3(0,0,0), 0.01f, 1, MAT_METAL);
    int right    = add_material(v3(0.85,0.65,0.2), v3(0,0,0), 0.2f, 1, MAT_METAL);
    int chrome   = add_material(v3(0.95,0.93,0.9), v3(0,0,0), 0.0f, 1, MAT_METAL);
    int glass    = add_material(v3(0.95,0.95,0.95), v3(0,0,0), 0, 1.5f, MAT_DIELECTRIC);
    /* Remove scene sun — rich procedural sky handles outdoor illumination */
    /* Small fill lights only */
    int fill = add_material(v3(0.6,0.7,0.9), v3(1.5,2,3), 0, 1, MAT_EMISSIVE);
    int fill2 = add_material(v3(0.9,0.8,0.6), v3(2,1.5,0.8), 0, 1, MAT_EMISSIVE);

    /* Gradient ground — checker of two grays */
    for (int ix = -3; ix <= 3; ix++) {
        for (int iz = -3; iz <= 3; iz++) {
            int mat = ((ix + iz) & 1) ? ground : ground2;
            add_sphere(v3((float)ix * 2.0f, -0.01f, (float)iz * 2.0f - 1.0f), 1.05f, mat);
        }
    }

    add_sphere(v3(0, 0.8f, -1), 1.2f, center);
    add_sphere(v3(-3.0f, 0.6f, -1.8f), 1.0f, left);
    add_sphere(v3(3.0f, 0.5f, -1.5f), 1.0f, right);
    add_sphere(v3(-1.5f, 0.4f, -2.5f), 0.55f, chrome);
    add_sphere(v3(1.8f, 0.35f, 0.2f), 0.4f, glass);
    /* Distant fill lights for subtle ambient */
    add_sphere(v3(-8, 10, 6), 0.8f, fill);
    add_sphere(v3(6, 8, 8), 0.5f, fill2);

    g_camera.eye = v3(0, 1.6f, 4.5f);
    g_camera.lookat = v3(0, 0.5f, -1);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 55.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 4.5f;
}

/* ── Scene 2: Glass & Light ── */
static void setup_glass_light(void) {
    int ground = add_material(v3(0.18,0.18,0.22), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int ground2= add_material(v3(0.35,0.35,0.40), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int glass1 = add_material(v3(0.98,0.98,0.98), v3(0,0,0), 0, 1.5f, MAT_DIELECTRIC);
    int glass2 = add_material(v3(0.98,0.98,0.98), v3(0,0,0), 0, 1.5f, MAT_DIELECTRIC);
    int metal  = add_material(v3(0.95,0.93,0.88), v3(0,0,0), 0.01f, 1, MAT_METAL);
    int light  = add_material(v3(1,1,0.95), v3(1.5,1.5,1), 0, 1, MAT_EMISSIVE);
    int red    = add_material(v3(0.9,0.15,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int blue   = add_material(v3(0.15,0.2,0.9), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int gold   = add_material(v3(0.95,0.85,0.3), v3(0,0,0), 0.05f, 1, MAT_METAL);

    /* Checker ground */
    for (int ix = -3; ix <= 3; ix++) {
        for (int iz = -3; iz <= 3; iz++) {
            int mat = ((ix + iz) & 1) ? ground : ground2;
            add_sphere(v3((float)ix * 1.5f, -0.01f, (float)iz * 1.5f - 1.0f), 0.8f, mat);
        }
    }

    add_sphere(v3(-1.8f, 0.7f, -1.5f), 1.0f, glass1);
    add_sphere(v3(1.8f, 0.4f, -1.0f), 0.7f, glass2);
    add_sphere(v3(0, 0.9f, -2.8f), 0.9f, metal);
    add_sphere(v3(-0.6f, 0.35f, -0.1f), 0.4f, red);
    add_sphere(v3(1.6f, 0.3f, -2.5f), 0.35f, blue);
    add_sphere(v3(-2.5f, 0.5f, -2.0f), 0.3f, gold);
    add_sphere(v3(8, 12, 8), 0.3f, light);

    g_camera.eye = v3(0, 1.5f, 4.0f);
    g_camera.lookat = v3(0, 0.5f, -1.2f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 55.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 4.0f;
}

/* ── Scene 3: Random Spheres (procedural) ── */
static void setup_random_spheres(void) {
    int colors[6];
    colors[0] = add_material(v3(0.95,0.15,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* red */
    colors[1] = add_material(v3(0.15,0.95,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* green */
    colors[2] = add_material(v3(0.15,0.15,0.95), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* blue */
    colors[3] = add_material(v3(0.95,0.95,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* yellow */
    colors[4] = add_material(v3(0.95,0.5,0.1), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);   /* orange */
    colors[5] = add_material(v3(0.75,0.15,0.75), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* purple */
    int metal = add_material(v3(0.95,0.93,0.88), v3(0,0,0), 0.03f, 1, MAT_METAL);
    int glass = add_material(v3(0.98,0.98,0.98), v3(0,0,0), 0, 1.5f, MAT_DIELECTRIC);
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

    /* Random field of spheres */
    unsigned int rng = 42;
    for (int i = 0; i < 50; i++) {
        float x = (randf(&rng) - 0.5f) * 8.0f;
        float z = (randf(&rng) - 0.5f) * 7.0f - 1.0f;
        float r = 0.12f + randf(&rng) * 0.40f;
        float y = r;
        int mat;
        float choice = randf(&rng);
        if (choice < 0.12f) mat = metal;
        else if (choice < 0.22f) mat = glass;
        else mat = colors[(int)(randf(&rng) * 6)];
        add_sphere(v3(x, y, z), r, mat);
    }

    /* Large center glass sphere */
    add_sphere(v3(0, 0.9f, -1.8f), 0.9f, glass);

    /* Sun far above */
    add_sphere(v3(8, 16, 12), 0.3f, sun);

    g_camera.eye = v3(0, 2.2f, 5.0f);
    g_camera.lookat = v3(0, 0.5f, -0.5f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 60.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 5.0f;
}

/* ── Scene 4: Checkerboard floor with columns ── */
static void setup_checkerboard(void) {
    int white = add_material(v3(0.85,0.85,0.85), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int black = add_material(v3(0.15,0.15,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    int red   = add_material(v3(0.8,0.15,0.15), v3(0,0,0), 0.05f, 1, MAT_METAL);
    int blue  = add_material(v3(0.15,0.2,0.8), v3(0,0,0), 0.1f, 1, MAT_METAL);
    int gold  = add_material(v3(0.95,0.85,0.3), v3(0,0,0), 0.02f, 1, MAT_METAL);
    int sun   = add_material(v3(1,1,1), v3(3,2.5,2), 0, 1, MAT_EMISSIVE);

    /* Checkerboard floor — overlapping spheres for a smooth surface */
    for (int ix = -7; ix <= 7; ix++) {
        for (int iz = -6; iz <= 6; iz++) {
            int mat = ((ix + iz) & 1) ? white : black;
            /* Radius (0.95) overlaps spacing (1.5) for a continuous floor */
            add_sphere(v3((float)ix * 1.5f, -0.70f, (float)iz * 1.5f - 2.0f), 0.95f, mat);
        }
    }

    /* Columns */
    for (int i = -2; i <= 2; i += 2) {
        add_sphere(v3((float)i * 2.5f, 1.5f, -2.0f + (float)i * 1.0f), 0.4f, red);
        add_sphere(v3((float)i * 2.5f, 3.0f, -2.0f + (float)i * 1.0f), 0.4f, red);
        add_sphere(v3((float)i * 2.5f, 4.5f, -2.0f + (float)i * 1.0f), 0.4f, red);
    }

    /* Decorative spheres */
    add_sphere(v3(0, 0.6f, -2.5f), 0.6f, gold);
    add_sphere(v3(-3.5f, 0.5f, -3.0f), 0.5f, blue);

    /* Distant sun, sky handles main illumination */
    add_sphere(v3(10, 18, 12), 0.5f, sun);

    g_camera.eye = v3(0, 1.8f, 5.0f);
    g_camera.lookat = v3(0, 1.0f, -2.0f);
    g_camera.up = v3(0, 1, 0);
    g_camera.fov = 55.0f;
    g_camera.aperture = 0.0f;
    g_camera.focus_dist = 5.0f;
}

/* ── Scene 5: Cosmic (abstract floating orbs with starfield dome) ── */
static void setup_cosmic(void) {
    int bg   = add_material(v3(0.03,0.02,0.10), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);  /* deep space */
    int bg2  = add_material(v3(0.05,0.03,0.12), v3(0.001,0.001,0.003), 0, 1, MAT_EMISSIVE); /* subtle nebula glow */
    int orb1 = add_material(v3(0.2,0.6,1.0), v3(0,0,0), 0, 1.3f, MAT_DIELECTRIC);
    int orb2 = add_material(v3(1.0,0.3,0.5), v3(0,0,0), 0, 1.4f, MAT_DIELECTRIC);
    int orb3 = add_material(v3(0.3,1.0,0.3), v3(0,0,0), 0, 1.3f, MAT_DIELECTRIC);
    int orb4 = add_material(v3(0.9,0.7,0.2), v3(0,0,0), 0, 1.2f, MAT_DIELECTRIC);
    int orb5 = add_material(v3(0.7,0.3,0.9), v3(0,0,0), 0, 1.35f, MAT_DIELECTRIC);
    int light1 = add_material(v3(1,0.85,0.6), v3(20,16,8), 0, 1, MAT_EMISSIVE);
    int light2 = add_material(v3(0.6,0.85,1), v3(8,12,25), 0, 1, MAT_EMISSIVE);
    int light3 = add_material(v3(0.9,0.6,1), v3(15,8,18), 0, 1, MAT_EMISSIVE);
    int light4 = add_material(v3(0.7,1.0,0.7), v3(8,12,7), 0, 1, MAT_EMISSIVE);
    int mirror = add_material(v3(1,1,1), v3(0,0,0), 0.0f, 1, MAT_METAL);
    int ground = add_material(v3(0.08,0.08,0.15), v3(0,0,0), 0, 1, MAT_LAMBERTIAN);
    /* Star colors */
    int star_warm  = add_material(v3(1.0,0.9,0.7), v3(9,6,3), 0, 1, MAT_EMISSIVE);
    int star_cool  = add_material(v3(0.7,0.8,1.0), v3(4,6,12), 0, 1, MAT_EMISSIVE);
    int star_red   = add_material(v3(1.0,0.4,0.3), v3(6,1,1), 0, 1, MAT_EMISSIVE);
    int star_blue  = add_material(v3(0.3,0.5,1.0), v3(2,3,10), 0, 1, MAT_EMISSIVE);
    int nebula     = add_material(v3(0.5,0.3,0.8), v3(0.6,0.2,1.2), 0, 1, MAT_EMISSIVE);
    int nebula2    = add_material(v3(0.3,0.6,0.5), v3(0.3,0.8,0.4), 0, 1, MAT_EMISSIVE);

    /* Dark dome with subtle pattern — alternating deep space patches */
    float R = 18.0f;
    for (int ix = -4; ix <= 4; ix++) {
        for (int iy = -2; iy <= 3; iy++) {
            float x = (float)ix * 2.8f;
            float y = (float)iy * 2.8f;
            float z = -R * 0.7f;
            float dist = sqrtf(x*x + y*y + z*z);
            if (dist > R - 1.5f) {
                int mat = ((ix + iy) & 1) ? bg : bg2;
                add_sphere(v3(x, y, z), 1.6f, mat);
            }
        }
    }
    /* Additional far-dome patches */
    add_sphere(v3(0, 0, -R * 0.95f), R * 0.65f, bg2);

    /* Stars scattered on dome interior */
    unsigned int rng = 137;
    int star_mats[] = { star_warm, star_cool, star_red, star_blue };
    for (int i = 0; i < 120; i++) {
        /* Random direction on sphere */
        float theta = 2.0f * PI * randf(&rng);
        float phi = acosf(2.0f * randf(&rng) - 1.0f);
        float sr = 12.0f + randf(&rng) * 6.0f;
        float sx = sr * sinf(phi) * cosf(theta);
        float sy = sr * sinf(phi) * sinf(theta);
        float sz = sr * cosf(phi) - 1.0f;  /* center around z=-1 */
        float size = 0.04f + randf(&rng) * 0.10f;
        int smat = star_mats[(int)(randf(&rng) * 4)];
        add_sphere(v3(sx, sy, sz), size, smat);
    }

    /* Nebula clouds */
    for (int i = 0; i < 8; i++) {
        float nx = (randf(&rng) - 0.5f) * 12.0f;
        float ny = (randf(&rng) - 0.5f) * 6.0f;
        float nz = -6.0f + (randf(&rng) - 0.5f) * 10.0f;
        int nm = (i & 1) ? nebula : nebula2;
        add_sphere(v3(nx, ny, nz), 0.8f + randf(&rng) * 1.2f, nm);
    }

    /* Ground disc */
    add_sphere(v3(0, -3.0f, -1.0f), 5.0f, ground);

    /* Floating orbs */
    add_sphere(v3(-1.5f, 0.8f, -2.0f), 1.2f, orb1);
    add_sphere(v3(1.8f, 0.3f, -1.8f), 0.9f, orb2);
    add_sphere(v3(0.3f, -0.1f, -3.0f), 0.7f, orb3);
    add_sphere(v3(-2.5f, 0.5f, -3.5f), 0.5f, mirror);
    add_sphere(v3(2.0f, -0.2f, -2.8f), 0.6f, orb4);
    add_sphere(v3(-0.8f, -0.4f, -1.5f), 0.4f, orb5);

    /* Colored lights — larger and brighter */
    add_sphere(v3(2.5f, 2.0f, -1.0f), 0.6f, light1);
    add_sphere(v3(-2.0f, 1.5f, -2.5f), 0.55f, light2);
    add_sphere(v3(1.0f, -1.5f, -1.5f), 0.5f, light3);
    add_sphere(v3(-1.5f, -0.5f, -1.0f), 0.35f, light4);

    g_camera.eye = v3(0, 0.3f, 3.2f);
    g_camera.lookat = v3(0, 0.2f, -2.0f);
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
        return sky_color(ray);
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

EMSCRIPTEN_KEEPALIVE
void destroy(void) {
    free_scene();
    if (g_accum) { free(g_accum); g_accum = NULL; }
    if (g_output) { free(g_output); g_output = NULL; }
    g_width = g_height = 0;
}
