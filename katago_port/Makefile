# Builds the subset of KataGo needed by NXGo (search + Eigen CPU backend) as libkatago.a for Switch.
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif
include $(DEVKITPRO)/libnx/switch_rules

KATAGO := ../third_party/katago/cpp
EIGEN  := ../third_party/eigen
BUILD  := build

KATAGO_SRCS := \
	core/global.cpp core/base64.cpp core/bsearch.cpp core/commandloop.cpp core/config_parser.cpp \
	core/datetime.cpp core/elo.cpp core/fancymath.cpp core/fileutils.cpp core/hash.cpp core/logger.cpp \
	core/mainargs.cpp core/makedir.cpp core/md5.cpp core/multithread.cpp core/parallel.cpp core/rand.cpp \
	core/rand_helpers.cpp core/sha2.cpp core/test.cpp core/threadsafecounter.cpp core/threadsafequeue.cpp \
	core/timer.cpp \
	game/board.cpp game/rules.cpp game/boardhistory.cpp game/graphhash.cpp \
	dataio/sgf.cpp dataio/loadmodel.cpp dataio/homedata.cpp dataio/files.cpp \
	neuralnet/nninputs.cpp neuralnet/sgfmetadata.cpp neuralnet/modelversion.cpp neuralnet/nneval.cpp \
	neuralnet/desc.cpp neuralnet/eigenbackend.cpp \
	search/timecontrols.cpp search/searchparams.cpp search/mutexpool.cpp search/search.cpp \
	search/searchnode.cpp search/searchresults.cpp search/searchhelpers.cpp search/searchexplorehelpers.cpp \
	search/searchmirror.cpp search/searchmultithreadhelpers.cpp search/searchnnhelpers.cpp \
	search/searchtimehelpers.cpp search/searchupdatehelpers.cpp search/asyncbot.cpp \
	search/distributiontable.cpp search/localpattern.cpp search/searchnodetable.cpp \
	search/subtreevaluebiastable.cpp search/evalcache.cpp search/patternbonustable.cpp \
	search/analysisdata.cpp search/reportedsearchvalues.cpp \
	program/gtpconfig.cpp program/setup.cpp program/playutils.cpp program/playsettings.cpp

OBJS := $(addprefix $(BUILD)/,$(KATAGO_SRCS:.cpp=.o)) $(BUILD)/switch_compat.o

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFINES  := -D__SWITCH__ -D__unix=1 -DBYTE_ORDER=1234 -DLITTLE_ENDIAN=1234 -DBIG_ENDIAN=4321 \
            -DUSE_EIGEN_BACKEND -DNDEBUG -DEIGEN_NO_DEBUG
INCLUDES := -include $(CURDIR)/include/switch_compat.h -I$(CURDIR)/include \
            -I$(DEVKITPRO)/libnx/include -I$(DEVKITPRO)/portlibs/switch/include -isystem $(EIGEN)
CXXFLAGS := -std=c++17 -O3 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDES)
CFLAGS   := -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -I$(DEVKITPRO)/libnx/include

all: $(BUILD)/libkatago.a

$(BUILD)/libkatago.a: $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/%.o: $(KATAGO)/%.cpp
	@mkdir -p $(dir $@)
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/switch_%.o: switch_%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
