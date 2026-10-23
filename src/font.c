#include "font.h"

#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H

struct font {
	FT_Face face;
	FT_Face fallback[4];
	int fallback_count;
	int     pt_size;
};

static FT_Library g_library;
static int        g_library_initialized;

int font_library_init(void) {
	if (g_library_initialized) return 0;
	if (FT_Init_FreeType(&g_library) != 0) return -1;
	g_library_initialized = 1;
	return 0;
}

void font_library_quit(void) {
	if (!g_library_initialized) return;
	FT_Done_FreeType(g_library);
	g_library = NULL;
	g_library_initialized = 0;
}

font_t *font_open(const char *path, int pt_size) {
	if (!g_library_initialized || !path || pt_size <= 0) return NULL;
	static const char sandbox_prefix[] = "$SANDBOX/";
	const size_t sandbox_prefix_len = sizeof(sandbox_prefix) - 1;
	char expanded[1024];
	const char *open_path = path;
	if (strncmp(path, sandbox_prefix, sandbox_prefix_len) == 0) {
		const char *sandbox = getenv("SANDBOX");
		if (sandbox == NULL || sandbox[0] == '\0') return NULL;
		int n = snprintf(expanded, sizeof(expanded), "%s/%s",
		                 sandbox, path + sandbox_prefix_len);
		if (n < 0 || (size_t)n >= sizeof(expanded)) return NULL;
		open_path = expanded;
	}
	font_t *f = calloc(1, sizeof(*f));
	if (!f) return NULL;
	if (FT_New_Face(g_library, open_path, 0, &f->face) != 0) {
		free(f);
		return NULL;
	}
	/* Matches SDL_ttf's TTF_OpenFont, which calls
	 * FT_Set_Char_Size(face, 0, ptsize * 64, 0, 0). */
	if (FT_Set_Char_Size(f->face, 0, pt_size * 64, 0, 0) != 0) {
		FT_Done_Face(f->face);
		free(f);
		return NULL;
	}
	f->pt_size = pt_size;
	return f;
}

/* Explicit opt-in: UI font discovery must still test each font's own cmap. */
int font_add_fallback(font_t *f, const char *path) {
    if (!f || !path || f->fallback_count >= 4) return -1;
    FT_Face face = NULL;
    if (FT_New_Face(g_library, path, 0, &face)) return -1;
    if (FT_Set_Char_Size(face, 0, f->pt_size * 64, 0, 0)) {
        FT_Done_Face(face); return -1;
    }
    f->fallback[f->fallback_count++] = face;
    return 0;
}

static int find_cjk_font(const char *root, int depth, int *budget,
                         char *out, size_t cap) {
    if (depth < 0 || *budget <= 0) return 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && *budget > 0 && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            found = find_cjk_font(path, depth - 1, budget, out, cap);
            continue;
        }
        const char *ext = strrchr(path, '.');
        if (!ext || (strcmp(ext,".ttf") && strcmp(ext,".ttc") && strcmp(ext,".otf"))) continue;
        --*budget;
        FT_Face face = NULL;
        if (FT_New_Face(g_library, path, 0, &face)) continue;
        if (FT_Get_Char_Index(face, 0x4e2d) && FT_Get_Char_Index(face, 0x6587)) {
            snprintf(out, cap, "%s", path); found = 1;
        }
        FT_Done_Face(face);
    }
    closedir(d);
    return found;
}

int font_enable_system_fallback(font_t *f) {
    static char path[1024];
    static int searched;
    if (!f) return -1;
    if (!searched) {
        /* Prefer the regular BB10 Simplified Chinese face over its bold peer. */
        const char *preferred[] = {
            "/usr/fonts/font_repository/monotype/MHeiS.GB18030_200910.ttf",
            "/usr/fonts/font_repository/monotype/MHeiM.GB18030_200910.ttf",
            NULL
        };
        for (int i = 0; preferred[i] && !path[0]; ++i) {
            FT_Face face = NULL;
            if (!FT_New_Face(g_library, preferred[i], 0, &face)) {
                if (FT_Get_Char_Index(face, 0x4e2d) && FT_Get_Char_Index(face, 0x6587))
                    snprintf(path, sizeof(path), "%s", preferred[i]);
                FT_Done_Face(face);
            }
        }
        int budget = 400;
        if (!path[0]) find_cjk_font("/usr/fonts", 4, &budget, path, sizeof(path));
        searched = 1;
    }
    return path[0] ? font_add_fallback(f, path) : -1;
}

static FT_Face glyph_face(font_t *f, uint32_t cp) {
    if (FT_Get_Char_Index(f->face, cp)) return f->face;
    for (int i = 0; i < f->fallback_count; ++i)
        if (FT_Get_Char_Index(f->fallback[i], cp)) return f->fallback[i];
    return f->face; /* Retain the primary font's missing-glyph indicator. */
}

void font_close(font_t *f) {
	if (!f) return;
	for (int i = 0; i < f->fallback_count; ++i) FT_Done_Face(f->fallback[i]);
	if (f->face) FT_Done_Face(f->face);
	free(f);
}

int font_line_skip(const font_t *f) {
	if (!f || !f->face) return 0;
	return (int)(f->face->size->metrics.height >> 6);
}

int font_is_fixed_width(const font_t *f) {
	if (!f || !f->face) return 0;
	return (f->face->face_flags & FT_FACE_FLAG_FIXED_WIDTH) ? 1 : 0;
}

int font_glyph_metrics(font_t *f, uint32_t codepoint,
                       int *minx, int *maxx,
                       int *miny, int *maxy,
                       int *advance) {
	if (!f || !f->face) return -1;
	FT_Face face = glyph_face(f, codepoint);
	FT_Set_Char_Size(face, 0, f->pt_size * 64, 0, 0);
	FT_Set_Transform(face, NULL, NULL);
	FT_UInt gi = FT_Get_Char_Index(face, codepoint);
	if (gi == 0) return -1;
	if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) != 0) return -1;
	const FT_Glyph_Metrics *m = &face->glyph->metrics;
	if (minx)    *minx    = (int)(m->horiBearingX >> 6);
	if (maxx)    *maxx    = (int)((m->horiBearingX + m->width) >> 6);
	if (maxy)    *maxy    = (int)(m->horiBearingY >> 6);
	if (miny)    *miny    = (int)((m->horiBearingY - m->height) >> 6);
	if (advance) *advance = (int)(m->horiAdvance >> 6);
	return 0;
}

static void apply_italic_transform(font_style_t style, FT_Face face) {
	if (style & FONT_STYLE_ITALIC) {
		FT_Matrix shear;
		shear.xx = 1 << 16;
		shear.xy = (FT_Fixed)(0.2 * (1 << 16));
		shear.yx = 0;
		shear.yy = 1 << 16;
		FT_Set_Transform(face, &shear, NULL);
	} else {
		FT_Set_Transform(face, NULL, NULL);
	}
}

bitmap_t *font_render_glyph_cell(font_t *f, uint32_t codepoint,
                                   font_style_t style,
                                   rgb_t fg, rgb_t bg, int cell_w, int cell_h) {
	if (!f || !f->face) return NULL;
	FT_Face face = glyph_face(f, codepoint);
	FT_Set_Char_Size(face, 0, f->pt_size * 64, 0, 0);

	apply_italic_transform(style, face);
	if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) != 0) return NULL;
	if (style & FONT_STYLE_BOLD) FT_GlyphSlot_Embolden(face->glyph);
	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return NULL;

	FT_GlyphSlot slot      = face->glyph;
	int          ascender  = (int)(f->face->size->metrics.ascender >> 6);
	int          line_skip = cell_h > 0 ? cell_h : font_line_skip(f);
	int          advance   = (int)(slot->advance.x >> 6);
	if (line_skip <= 0) line_skip = 1;
	if (advance   <= 0) advance   = 1;
    if (cell_w > 0) advance = cell_w;
    /* A fallback face can have different metrics. Fit its ink inside the
     * terminal cell, keeping the primary font's line pitch and ASCII grid. */
    if (face != f->face && cell_w > 0) {
        int w = (int)slot->bitmap.width, h = (int)slot->bitmap.rows;
        if (w > advance || h > line_skip) {
            FT_F26Dot6 size = f->pt_size * 64;
            if (w > advance) size = size * advance / w;
            if (h > line_skip && size > f->pt_size * 64 * line_skip / h)
                size = f->pt_size * 64 * line_skip / h;
            if (size < 64) size = 64;
            if (FT_Set_Char_Size(face, 0, size, 0, 0) ||
                FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT)) return NULL;
            if (style & FONT_STYLE_BOLD) FT_GlyphSlot_Embolden(face->glyph);
            if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return NULL;
        }
    }

	bitmap_t *out = bitmap_alloc(advance, line_skip, BITMAP_FMT_RGBA8888);
	if (!out) return NULL;
	bitmap_fill_rect(out, NULL, bg);

	if (slot->bitmap.buffer && slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
		int gx0 = slot->bitmap_left;
		int gy0 = ascender - slot->bitmap_top;
        if (face != f->face && cell_w > 0) {
            /* Preserve the fallback's side bearings (notably fullwidth
             * punctuation), centering its advance inside the grid span. */
            gx0 = slot->bitmap_left + (advance - (int)(slot->advance.x >> 6)) / 2;
            if (gx0 < 0) gx0 = 0;
            if (gx0 + (int)slot->bitmap.width > advance)
                gx0 = advance - (int)slot->bitmap.width;
            if (gy0 < 0) gy0 = 0;
            if (gy0 + (int)slot->bitmap.rows > line_skip)
                gy0 = line_skip - (int)slot->bitmap.rows;
        }
		int gw  = (int)slot->bitmap.width;
		int gh  = (int)slot->bitmap.rows;
		int pitch = slot->bitmap.pitch;
		for (int y = 0; y < gh; ++y) {
			int dy = gy0 + y;
			if (dy < 0 || dy >= out->h) continue;
			const uint8_t *srow = slot->bitmap.buffer + (size_t)y * pitch;
			uint8_t       *drow = out->pixels + (size_t)dy * out->stride;
			for (int x = 0; x < gw; ++x) {
				int dx = gx0 + x;
				if (dx < 0 || dx >= out->w) continue;
				uint8_t a = srow[x];
				uint8_t *p = drow + (size_t)dx * 4;
				/* BGRA in memory: see bitmap.h for the byte-order rationale. */
				p[0] = (uint8_t)((fg.b * a + bg.b * (255 - a)) / 255);
				p[1] = (uint8_t)((fg.g * a + bg.g * (255 - a)) / 255);
				p[2] = (uint8_t)((fg.r * a + bg.r * (255 - a)) / 255);
				p[3] = 0xFF;
			}
		}
	}

	if (style & FONT_STYLE_UNDERLINE) {
		int ul_thick = f->pt_size / 12;
		if (ul_thick < 1) ul_thick = 1;
		int ul_y = ascender + 1;
		if (ul_y + ul_thick > out->h) ul_y = out->h - ul_thick;
		if (ul_y < 0) ul_y = 0;
		rect_t r;
		r.x = 0;
		r.y = ul_y;
		r.w = out->w;
		r.h = ul_thick;
		bitmap_fill_rect(out, &r, fg);
	}

	return out;
}

bitmap_t *font_render_glyph_shaded(font_t *f, uint32_t codepoint,
                                   font_style_t style, rgb_t fg, rgb_t bg) {
    return font_render_glyph_cell(f, codepoint, style, fg, bg, 0, 0);
}
