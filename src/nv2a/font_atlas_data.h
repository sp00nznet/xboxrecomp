/**
 * Font atlas placeholder - game-specific data should be provided
 * by the consuming project. Override by defining these before including,
 * or provide your own font_atlas_data.h in an earlier include path.
 */
#ifndef FONT_ATLAS_DATA_H
#define FONT_ATLAS_DATA_H

#ifndef FONT_ATLAS_WIDTH
#define FONT_ATLAS_WIDTH  256
#endif
#ifndef FONT_ATLAS_HEIGHT
#define FONT_ATLAS_HEIGHT 128
#endif
#ifndef FONT_ATLAS_SIZE
#define FONT_ATLAS_SIZE   0
#endif

#ifndef FONT_ATLAS_DATA_PROVIDED
static const unsigned char font_atlas_dxt5[] = { 0 };
#endif

#endif /* FONT_ATLAS_DATA_H */
