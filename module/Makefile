# define the module name - make sure name is max 8 characters
MODULE_NAME = soup

# engine/ is Soup Engine's freestanding core, copied unchanged from upstream
# (see engine/UPSTREAM). Only the JPEG preview path is linked for now.
ENGINE_OBJS = se wb cr2 color lut stock grade look jpeg_dec jpeg_enc
MODULE_OBJS = $(BUILD_DIR)/soup.o $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(ENGINE_OBJS)))
vpath %.c engine

TOP_DIR = ../..

# include modules environment
include $(TOP_DIR)/modules/Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/module_complete

# The engine isn't written against ML's warning set; keep its warnings
# visible without failing the build, so it can stay byte-identical to upstream.
# ML's -ffast-math rewrites the engine's exp2 loops into __powisf2, which the
# 80D build doesn't export, and would make the look's float bake differ from
# the desktop build the web tool previews with.
$(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(ENGINE_OBJS))): CFLAGS += -Wno-error -fno-fast-math -fno-single-precision-constant
