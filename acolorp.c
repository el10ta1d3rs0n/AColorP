/*
 * acolorp - generate a color palette from an image and write ready-to-use
 * theme files (like pywal / wallust). Built-in templates are used by default;
 * extra custom *.tpl templates are optional.
 *
 * Build:  gcc -O2 -Wall -Wextra -o acolorp acolorp.c -lm
 * Needs:  stb_image.h in the same folder (or on the include path)
 *
 * Template variables (inside *.tpl files):
 *   {{color0}} .. {{color7}}   -> #RRGGBB
 *   {{background}} {{foreground}}
 * Modifiers:
 *   {{color1.hex}}  -> RRGGBB   (no '#', e.g. for Hyprland rgb(...))
 *   {{color1.rgb}}  -> R,G,B    (decimal, e.g. for rgba(...))
 */
#define _POSIX_C_SOURCE 200809L

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BINS 32768
#define PALETTE_SIZE 8
#define MIN_COLOR_DISTANCE 32
#define MIN_BRIGHTNESS 40

typedef struct {
    unsigned long count;
    unsigned long long r, g, b;
} Bin;

typedef struct {
    int index;
    unsigned long count;
} BinRef;

typedef struct {
    unsigned char r, g, b;
    unsigned long count;
} Color;

typedef struct {
    Color colors[PALETTE_SIZE];
    Color background;
    Color foreground;
} Palette;

typedef enum { TARGET_NONE, TARGET_HYPRLAND, TARGET_SWAY } Target;

static void usage(const char *program) {
    fprintf(stderr,
        "Usage: %s [options] image-file\n\n"
        "  --output DIR      where files are written "
        "(default: ~/.config/acolorp)\n"
        "  --only LIST       write only these built-in files "
        "(comma-separated, e.g. sway,kitty)\n"
        "                    use \"none\" to skip all built-in files\n"
        "  --list            show the built-in files and exit\n"
        "  --reload NAME     reload compositor afterwards "
        "(hyprland or sway)\n"
        "  --templates DIR   optional: also render your own *.tpl files\n\n"
        "Without --only, all built-in files are written.\n",
        program);
}

/* ---------- filesystem helpers ---------- */

static int mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof buf) {
        fprintf(stderr, "Invalid directory path.\n");
        return -1;
    }
    memcpy(buf, path, len + 1);

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                perror(buf);
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        perror(buf);
        return -1;
    }
    return 0;
}

static int resolve_output_dir(char *out, size_t out_size,
                              const char *requested) {
    int n;
    if (requested) {
        n = snprintf(out, out_size, "%s", requested);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            fprintf(stderr, "HOME is not set; specify --output DIR.\n");
            return -1;
        }
        n = snprintf(out, out_size, "%s/.config/acolorp", home);
    }
    if (n < 0 || (size_t)n >= out_size) {
        fprintf(stderr, "Output directory path is too long.\n");
        return -1;
    }
    return mkdir_p(out);
}

/* ---------- color helpers ---------- */

static unsigned int brightness_of(unsigned char r, unsigned char g,
                                  unsigned char b) {
    return (54U * r + 183U * g + 19U * b) / 256U;
}

static unsigned int color_distance_squared(const Color *a, const Color *b) {
    int dr = (int)a->r - (int)b->r;
    int dg = (int)a->g - (int)b->g;
    int db = (int)a->b - (int)b->b;
    return (unsigned int)(dr * dr + dg * dg + db * db);
}

/* Blend toward white by t (0..1). */
static Color lighten(Color c, double t) {
    if (t > 1.0) t = 1.0;
    c.r = (unsigned char)(c.r + (255 - c.r) * t + 0.5);
    c.g = (unsigned char)(c.g + (255 - c.g) * t + 0.5);
    c.b = (unsigned char)(c.b + (255 - c.b) * t + 0.5);
    return c;
}

/* Scale toward black by factor (0..1). */
static Color darken(Color c, double factor) {
    c.r = (unsigned char)(c.r * factor + 0.5);
    c.g = (unsigned char)(c.g * factor + 0.5);
    c.b = (unsigned char)(c.b * factor + 0.5);
    return c;
}

static int compare_binref_desc(const void *a, const void *b) {
    const BinRef *x = a, *y = b;
    if (x->count < y->count) return 1;
    if (x->count > y->count) return -1;
    return x->index - y->index; /* stable, deterministic */
}

static void collect_bins(const unsigned char *image,
                         unsigned long long pixel_count,
                         unsigned int min_brightness, Bin *bins) {
    memset(bins, 0, BINS * sizeof *bins);
    for (unsigned long long i = 0; i < pixel_count; i++) {
        unsigned char r = image[i * 3];
        unsigned char g = image[i * 3 + 1];
        unsigned char b = image[i * 3 + 2];

        if (brightness_of(r, g, b) < min_brightness)
            continue;

        int index = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        bins[index].count++;
        bins[index].r += r;
        bins[index].g += g;
        bins[index].b += b;
    }
}

/* Returns number of distinct colors picked (0 if image has no usable pixels). */
static int pick_colors(const Bin *bins, Color out[PALETTE_SIZE]) {
    BinRef *refs = malloc(BINS * sizeof *refs);
    if (!refs) return 0;

    int n = 0;
    for (int i = 0; i < BINS; i++) {
        if (bins[i].count > 0) {
            refs[n].index = i;
            refs[n].count = bins[i].count;
            n++;
        }
    }
    qsort(refs, (size_t)n, sizeof *refs, compare_binref_desc);

    const unsigned int min_d2 = MIN_COLOR_DISTANCE * MIN_COLOR_DISTANCE;
    int picked = 0;

    for (int k = 0; k < n && picked < PALETTE_SIZE; k++) {
        const Bin *bin = &bins[refs[k].index];
        Color cand = {
            .r = (unsigned char)(bin->r / bin->count),
            .g = (unsigned char)(bin->g / bin->count),
            .b = (unsigned char)(bin->b / bin->count),
            .count = bin->count
        };

        int similar = 0;
        for (int i = 0; i < picked; i++) {
            if (color_distance_squared(&cand, &out[i]) < min_d2) {
                similar = 1;
                break;
            }
        }
        if (!similar)
            out[picked++] = cand;
    }

    free(refs);
    return picked;
}

/* Fill missing slots with lighter variants of the colors we found. */
static void fill_palette(Color colors[PALETTE_SIZE], int found) {
    for (int i = found; i < PALETTE_SIZE; i++) {
        Color base = colors[i % found];
        int round = i / found; /* 1, 2, ... */
        colors[i] = lighten(base, 0.2 * round);
        colors[i].count = 0;
    }
}

static void derive_background_foreground(Palette *pal) {
    int darkest = 0, lightest = 0;
    for (int i = 1; i < PALETTE_SIZE; i++) {
        const Color *c = &pal->colors[i];
        if (brightness_of(c->r, c->g, c->b) <
            brightness_of(pal->colors[darkest].r, pal->colors[darkest].g,
                          pal->colors[darkest].b))
            darkest = i;
        if (brightness_of(c->r, c->g, c->b) >
            brightness_of(pal->colors[lightest].r, pal->colors[lightest].g,
                          pal->colors[lightest].b))
            lightest = i;
    }
    pal->background = darken(pal->colors[darkest], 0.20);
    pal->foreground = lighten(pal->colors[lightest], 0.80);
    pal->background.count = pal->foreground.count = 0;
}

/* ---------- template rendering ---------- */

/* Resolve "color3", "background", "foreground". */
static const Color *lookup_color(const Palette *pal, const char *name) {
    if (strcmp(name, "background") == 0) return &pal->background;
    if (strcmp(name, "foreground") == 0) return &pal->foreground;
    if (strncmp(name, "color", 5) == 0 && name[5] >= '0' &&
        name[5] < '0' + PALETTE_SIZE && name[6] == '\0')
        return &pal->colors[name[5] - '0'];
    return NULL;
}

/*
 * Try to expand a token starting at "{{". On success writes the value to
 * `out`, sets *consumed to the token length and returns 0. Returns 1 if it
 * is not a known token (caller prints it literally).
 */
static int expand_token(FILE *out, const char *p, const Palette *pal,
                        size_t *consumed) {
    const char *close = strstr(p + 2, "}}");
    if (!close) return 1;

    size_t inner_len = (size_t)(close - (p + 2));
    char inner[64];
    if (inner_len == 0 || inner_len >= sizeof inner) return 1;
    memcpy(inner, p + 2, inner_len);
    inner[inner_len] = '\0';

    char *modifier = strchr(inner, '.');
    if (modifier) *modifier++ = '\0';

    const Color *c = lookup_color(pal, inner);
    if (!c) return 1;

    int rc;
    if (!modifier)
        rc = fprintf(out, "#%02X%02X%02X", c->r, c->g, c->b);
    else if (strcmp(modifier, "hex") == 0)
        rc = fprintf(out, "%02X%02X%02X", c->r, c->g, c->b);
    else if (strcmp(modifier, "rgb") == 0)
        rc = fprintf(out, "%d,%d,%d", c->r, c->g, c->b);
    else
        return 1;

    if (rc < 0) return -1;
    *consumed = (size_t)(close - p) + 2;
    return 0;
}

static int render_line(FILE *out, const char *line, const Palette *pal) {
    const char *p = line;

    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            size_t consumed = 0;
            int r = expand_token(out, p, pal, &consumed);
            if (r < 0) return -1;
            if (r == 0) {
                p += consumed;
                continue;
            }
        }
        if (fputc((unsigned char)*p, out) == EOF) return -1;
        p++;
    }
    return 0;
}

/* Finish a temp file and atomically move it into place. */
static int finish_file(FILE *out, const char *temp_path,
                       const char *output_path, int failed) {
    if (fclose(out) != 0) failed = 1;
    if (failed) {
        fprintf(stderr, "Failed to write: %s\n", output_path);
        unlink(temp_path);
        return -1;
    }
    if (rename(temp_path, output_path) != 0) {
        perror("rename");
        unlink(temp_path);
        return -1;
    }
    printf("Wrote %s\n", output_path);
    return 0;
}

static int open_temp(FILE **out, char *temp_path, size_t size,
                     const char *output_path) {
    int n = snprintf(temp_path, size, "%s.tmp.%ld", output_path,
                     (long)getpid());
    if (n < 0 || (size_t)n >= size) {
        fprintf(stderr, "Output path is too long.\n");
        return -1;
    }
    *out = fopen(temp_path, "w");
    if (!*out) {
        perror(temp_path);
        return -1;
    }
    return 0;
}

static int render_template_file(const char *template_path,
                                const char *output_path,
                                const Palette *pal) {
    FILE *in = fopen(template_path, "r");
    if (!in) {
        perror(template_path);
        return -1;
    }

    char temp_path[PATH_MAX];
    FILE *out;
    if (open_temp(&out, temp_path, sizeof temp_path, output_path) < 0) {
        fclose(in);
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    int failed = 0;

    while (getline(&line, &capacity, in) != -1) {
        if (render_line(out, line, pal) < 0) {
            failed = 1;
            break;
        }
    }
    if (ferror(in)) failed = 1;

    free(line);
    fclose(in);
    return finish_file(out, temp_path, output_path, failed);
}

/* ---------- built-in templates ---------- */

typedef struct {
    const char *name;     /* short name used with --only */
    const char *filename;
    const char *text;
} BuiltinTemplate;

static const BuiltinTemplate BUILTIN_TEMPLATES[] = {
    { "hyprland", "colors-hyprland.conf",
      "# Generated by acolorp - do not edit\n"
      "$background = rgb({{background.hex}})\n"
      "$foreground = rgb({{foreground.hex}})\n"
      "$color0 = rgb({{color0.hex}})\n"
      "$color1 = rgb({{color1.hex}})\n"
      "$color2 = rgb({{color2.hex}})\n"
      "$color3 = rgb({{color3.hex}})\n"
      "$color4 = rgb({{color4.hex}})\n"
      "$color5 = rgb({{color5.hex}})\n"
      "$color6 = rgb({{color6.hex}})\n"
      "$color7 = rgb({{color7.hex}})\n"
      "\n"
      "general {\n"
      "    col.active_border = $color1 $color4 45deg\n"
      "    col.inactive_border = $background\n"
      "}\n" },

    { "sway", "colors-sway.conf",
      "# Generated by acolorp - do not edit\n"
      "set $background {{background}}\n"
      "set $foreground {{foreground}}\n"
      "set $color0 {{color0}}\n"
      "set $color1 {{color1}}\n"
      "set $color2 {{color2}}\n"
      "set $color3 {{color3}}\n"
      "set $color4 {{color4}}\n"
      "set $color5 {{color5}}\n"
      "set $color6 {{color6}}\n"
      "set $color7 {{color7}}\n"
      "\n"
      "# class                 border  backgr.      text         indicator child_border\n"
      "client.focused          $color1 $color1      $background  $color4   $color1\n"
      "client.focused_inactive $color0 $background  $foreground  $color0   $color0\n"
      "client.unfocused        $color0 $background  $foreground  $color0   $color0\n" },

    /* For waybar, swaync, GTK-style CSS: @define-color name; */
    { "gtk", "colors-gtk.css",
      "/* Generated by acolorp - do not edit */\n"
      "@define-color background {{background}};\n"
      "@define-color foreground {{foreground}};\n"
      "@define-color color0 {{color0}};\n"
      "@define-color color1 {{color1}};\n"
      "@define-color color2 {{color2}};\n"
      "@define-color color3 {{color3}};\n"
      "@define-color color4 {{color4}};\n"
      "@define-color color5 {{color5}};\n"
      "@define-color color6 {{color6}};\n"
      "@define-color color7 {{color7}};\n" },

    /* For web CSS: var(--color1) */
    { "css", "colors.css",
      "/* Generated by acolorp - do not edit */\n"
      ":root {\n"
      "  --background: {{background}};\n"
      "  --foreground: {{foreground}};\n"
      "  --color0: {{color0}};\n"
      "  --color1: {{color1}};\n"
      "  --color2: {{color2}};\n"
      "  --color3: {{color3}};\n"
      "  --color4: {{color4}};\n"
      "  --color5: {{color5}};\n"
      "  --color6: {{color6}};\n"
      "  --color7: {{color7}};\n"
      "}\n" },

    { "sh", "colors.sh",
      "# Generated by acolorp - do not edit\n"
      "background='{{background}}'\n"
      "foreground='{{foreground}}'\n"
      "color0='{{color0}}'\n"
      "color1='{{color1}}'\n"
      "color2='{{color2}}'\n"
      "color3='{{color3}}'\n"
      "color4='{{color4}}'\n"
      "color5='{{color5}}'\n"
      "color6='{{color6}}'\n"
      "color7='{{color7}}'\n" },

    { "json", "colors.json",
      "{\n"
      "  \"background\": \"{{background}}\",\n"
      "  \"foreground\": \"{{foreground}}\",\n"
      "  \"colors\": [\n"
      "    \"{{color0}}\", \"{{color1}}\", \"{{color2}}\", \"{{color3}}\",\n"
      "    \"{{color4}}\", \"{{color5}}\", \"{{color6}}\", \"{{color7}}\"\n"
      "  ]\n"
      "}\n" },

    /* kitty: add  include ~/.config/acolorp/colors-kitty.conf */
    { "kitty", "colors-kitty.conf",
      "# Generated by acolorp - do not edit\n"
      "background {{background}}\n"
      "foreground {{foreground}}\n"
      "color0 {{color0}}\n"
      "color1 {{color1}}\n"
      "color2 {{color2}}\n"
      "color3 {{color3}}\n"
      "color4 {{color4}}\n"
      "color5 {{color5}}\n"
      "color6 {{color6}}\n"
      "color7 {{color7}}\n" },
};

#define BUILTIN_COUNT \
    (sizeof BUILTIN_TEMPLATES / sizeof BUILTIN_TEMPLATES[0])

static int builtin_selected[BUILTIN_COUNT];
static int only_given = 0; /* 1 once --only was used */

static int builtin_enabled(size_t i) {
    return !only_given || builtin_selected[i];
}

static int builtin_enabled_by_name(const char *name) {
    for (size_t i = 0; i < BUILTIN_COUNT; i++)
        if (strcmp(BUILTIN_TEMPLATES[i].name, name) == 0)
            return builtin_enabled(i);
    return 0;
}

static void list_builtins(void) {
    printf("Built-in files (use with --only):\n");
    for (size_t i = 0; i < BUILTIN_COUNT; i++)
        printf("  %-9s -> %s\n", BUILTIN_TEMPLATES[i].name,
               BUILTIN_TEMPLATES[i].filename);
}

/* Parse "sway,kitty" or "colors.json". Returns -1 on unknown name. */
static int parse_only(const char *list) {
    char buf[256];
    if (strlen(list) >= sizeof buf) {
        fprintf(stderr, "--only list is too long.\n");
        return -1;
    }
    strcpy(buf, list);
    only_given = 1;

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, "none") == 0)
            continue;

        int found = 0;
        for (size_t i = 0; i < BUILTIN_COUNT; i++) {
            if (strcmp(tok, BUILTIN_TEMPLATES[i].name) == 0 ||
                strcmp(tok, BUILTIN_TEMPLATES[i].filename) == 0) {
                builtin_selected[i] = 1;
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Unknown built-in file: %s\n\n", tok);
            list_builtins();
            return -1;
        }
    }
    return 0;
}

static int render_builtin_templates(const char *output_dir,
                                    const Palette *pal) {
    int failed = 0;

    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (!builtin_enabled(i))
            continue;

        char output_path[PATH_MAX], temp_path[PATH_MAX];
        FILE *out;

        int n = snprintf(output_path, sizeof output_path, "%s/%s",
                         output_dir, BUILTIN_TEMPLATES[i].filename);
        if (n < 0 || (size_t)n >= sizeof output_path) {
            fprintf(stderr, "Output path is too long.\n");
            failed = 1;
            continue;
        }
        if (open_temp(&out, temp_path, sizeof temp_path, output_path) < 0) {
            failed = 1;
            continue;
        }
        int bad = render_line(out, BUILTIN_TEMPLATES[i].text, pal) < 0;
        if (finish_file(out, temp_path, output_path, bad) < 0)
            failed = 1;
    }
    return failed ? -1 : 0;
}

static int render_templates(const char *template_dir, const char *output_dir,
                            const Palette *pal) {
    DIR *dir = opendir(template_dir);
    if (!dir) {
        perror(template_dir);
        return -1;
    }

    struct dirent *entry;
    int rendered = 0, failed = 0;
    const char suffix[] = ".tpl";
    const size_t suffix_len = sizeof suffix - 1;

    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len <= suffix_len ||
            strcmp(entry->d_name + name_len - suffix_len, suffix) != 0)
            continue;

        char input_path[PATH_MAX], output_path[PATH_MAX];
        int n = snprintf(input_path, sizeof input_path, "%s/%s",
                         template_dir, entry->d_name);
        if (n < 0 || n >= (int)sizeof input_path) {
            fprintf(stderr, "Template path is too long.\n");
            failed = 1;
            continue;
        }

        n = snprintf(output_path, sizeof output_path, "%s/%.*s", output_dir,
                     (int)(name_len - suffix_len), entry->d_name);
        if (n < 0 || n >= (int)sizeof output_path) {
            fprintf(stderr, "Rendered file path is too long.\n");
            failed = 1;
            continue;
        }

        if (render_template_file(input_path, output_path, pal) < 0)
            failed = 1;
        else
            rendered++;
    }
    closedir(dir);

    if (rendered == 0) {
        fprintf(stderr, "No .tpl files found in %s.\n", template_dir);
        return -1;
    }
    return failed ? -1 : 0;
}

/* ---------- compositor reload ---------- */

static int reload_compositor(Target target) {
    char *hypr_args[] = {"hyprctl", "reload", NULL};
    char *sway_args[] = {"swaymsg", "reload", NULL};
    const char *program;
    char **args;

    if (target == TARGET_HYPRLAND) {
        program = "hyprctl";
        args = hypr_args;
    } else if (target == TARGET_SWAY) {
        program = "swaymsg";
        args = sway_args;
    } else {
        return 0;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return -1;
    }
    if (child == 0) {
        execvp(program, args);
        perror(program);
        _exit(127);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s reload failed.\n", program);
        return -1;
    }
    printf("Reloaded %s\n", program);
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    Target target = TARGET_NONE;
    const char *template_dir = NULL;
    const char *requested_output_dir = NULL;
    const char *image_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--templates") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--templates requires a directory.\n");
                return 1;
            }
            template_dir = argv[i];
        } else if (strcmp(argv[i], "--output") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--output requires a directory.\n");
                return 1;
            }
            requested_output_dir = argv[i];
        } else if (strcmp(argv[i], "--reload") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--reload requires hyprland or sway.\n");
                return 1;
            }
            if (strcmp(argv[i], "hyprland") == 0)
                target = TARGET_HYPRLAND;
            else if (strcmp(argv[i], "sway") == 0)
                target = TARGET_SWAY;
            else {
                fprintf(stderr, "Unknown compositor: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--only") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--only requires a list, e.g. sway,kitty.\n");
                return 1;
            }
            if (parse_only(argv[i]) < 0)
                return 1;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_builtins();
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            if (image_path) {
                fprintf(stderr, "Only one image file may be specified.\n");
                usage(argv[0]);
                return 1;
            }
            image_path = argv[i];
        }
    }

    if (!image_path) {
        usage(argv[0]);
        return 1;
    }

    int width, height, channels;
    unsigned char *image =
        stbi_load(image_path, &width, &height, &channels, 3);
    if (!image) {
        fprintf(stderr, "Could not load image: %s\n", stbi_failure_reason());
        return 1;
    }
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Invalid image dimensions.\n");
        stbi_image_free(image);
        return 1;
    }

    unsigned long long pixel_count =
        (unsigned long long)width * (unsigned long long)height;

    Bin *bins = malloc(BINS * sizeof *bins);
    if (!bins) {
        fprintf(stderr, "Could not allocate memory.\n");
        stbi_image_free(image);
        return 1;
    }

    Palette pal;
    memset(&pal, 0, sizeof pal);

    collect_bins(image, pixel_count, MIN_BRIGHTNESS, bins);
    int found = pick_colors(bins, pal.colors);
    if (found == 0) {
        /* Very dark image: retry without the brightness filter. */
        collect_bins(image, pixel_count, 0, bins);
        found = pick_colors(bins, pal.colors);
    }

    free(bins);
    stbi_image_free(image);

    if (found == 0) {
        fprintf(stderr, "No colors found in image.\n");
        return 1;
    }

    fill_palette(pal.colors, found);
    derive_background_foreground(&pal);

    printf("Palette from %s (%d distinct, %d generated):\n", image_path,
           found, PALETTE_SIZE - found);
    for (int i = 0; i < PALETTE_SIZE; i++)
        printf("  color%d     #%02X%02X%02X\n", i, pal.colors[i].r,
               pal.colors[i].g, pal.colors[i].b);
    printf("  background #%02X%02X%02X\n", pal.background.r,
           pal.background.g, pal.background.b);
    printf("  foreground #%02X%02X%02X\n", pal.foreground.r,
           pal.foreground.g, pal.foreground.b);

    char output_dir[PATH_MAX];
    if (resolve_output_dir(output_dir, sizeof output_dir,
                           requested_output_dir) < 0)
        return 1;

    if (render_builtin_templates(output_dir, &pal) < 0)
        return 1;

    if (template_dir && render_templates(template_dir, output_dir, &pal) < 0)
        return 1;

    if (builtin_enabled_by_name("hyprland") ||
        builtin_enabled_by_name("sway"))
        printf("\nTo use the colors, add ONE line to your config "
               "(only once):\n");
    if (builtin_enabled_by_name("hyprland"))
        printf("  Hyprland: source = %s/colors-hyprland.conf\n", output_dir);
    if (builtin_enabled_by_name("sway"))
        printf("  Sway:     include %s/colors-sway.conf\n", output_dir);

    if (target != TARGET_NONE && reload_compositor(target) < 0)
        return 1;

    return 0;
}
