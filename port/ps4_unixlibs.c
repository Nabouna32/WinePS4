/* WinePS4: the Unix libraries linked into the app, for ps4_dlopen/ps4_dlsym (ps4_wine_port.c).
 *
 * win32u.so and wineps4.so are Unix halves of builtin DLLs: ntdll looks up their
 * __wine_unix_call_funcs (renamed when linked, see app/CMakeLists.txt). libfreetype.so.6 is what
 * win32u's freetype.c loads the FreeType functions from. */
#include <stddef.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_LCD_FILTER_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_TRIGONOMETRY_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_WINFONTS_H

struct ps4_symbol { const char *name; void *addr; };
struct ps4_unixlib { const char *name; const struct ps4_symbol *symbols; };

extern const void *win32u_unix_call_funcs[];  /* (no 32-bit table: WoW64 uses the 64-bit win32u) */
extern const void *wineps4drv_unix_call_funcs[], *wineps4drv_unix_call_wow64_funcs[];

static const struct ps4_symbol win32u_symbols[] =
{
    { "__wine_unix_call_funcs", win32u_unix_call_funcs },
    { NULL, NULL }
};

static const struct ps4_symbol wineps4drv_symbols[] =
{
    { "__wine_unix_call_funcs", wineps4drv_unix_call_funcs },
    { "__wine_unix_call_wow64_funcs", wineps4drv_unix_call_wow64_funcs },
    { NULL, NULL }
};

#define FT(f) { #f, (void *)f }
static const struct ps4_symbol freetype_symbols[] =
{
    FT(FT_Done_Face), FT(FT_Get_Char_Index), FT(FT_Get_First_Char), FT(FT_Get_Next_Char),
    FT(FT_Get_Sfnt_Name), FT(FT_Get_Sfnt_Name_Count), FT(FT_Get_Sfnt_Table),
    FT(FT_Get_TrueType_Engine_Type), FT(FT_Get_WinFNT_Header), FT(FT_Init_FreeType),
    FT(FT_Library_SetLcdFilter), FT(FT_Library_Version), FT(FT_Load_Glyph), FT(FT_Load_Sfnt_Table),
    FT(FT_Matrix_Multiply), FT(FT_MulDiv), FT(FT_MulFix), FT(FT_New_Face), FT(FT_New_Memory_Face),
    FT(FT_Outline_Embolden), FT(FT_Outline_Get_Bitmap), FT(FT_Outline_Get_CBox),
    FT(FT_Outline_Transform), FT(FT_Outline_Translate), FT(FT_Property_Set), FT(FT_Render_Glyph),
    FT(FT_Set_Charmap), FT(FT_Set_Pixel_Sizes), FT(FT_Vector_Length), FT(FT_Vector_Transform),
    FT(FT_Vector_Unit), FT(FT_Done_FreeType), FT(FT_Get_Kerning), FT(FT_Outline_Get_Orientation),
    FT(FT_Get_Glyph_Name), FT(FT_GlyphSlot_Embolden), FT(FT_Select_Charmap),
    { NULL, NULL }
};

const struct ps4_unixlib ps4_unixlibs[] =
{
    { "win32u.so", win32u_symbols },
    { "wineps4.so", wineps4drv_symbols },
    { "libfreetype.so.6", freetype_symbols },
    { NULL, NULL }
};
