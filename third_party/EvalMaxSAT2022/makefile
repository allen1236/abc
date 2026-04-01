.PHONY: help all so static clean

CXX ?= g++
AR  ?= ar

OUT_SO := libipamirEvalMaxSAT2022.so
OUT_A  := libipamirEvalMaxSAT2022.a

INCLUDES := -Ilib/cadical/src -Ilib/EvalMaxSAT/src -Ilib/MaLib/src
LIBS := -lz -lpthread

CXXFLAGS ?= -O3 -DNDEBUG -std=c++17 -Wall

CADICAL_CPP := lib/cadical/src/*.cpp
EVAL_CPP    := lib/EvalMaxSAT/src/*.cpp
MALIB_CPP   := lib/MaLib/src/*.cpp

help:
	@echo "Targets:"
	@echo "  make so      - build $(OUT_SO) (used by abc &minr)"
	@echo "  make static  - build $(OUT_A)"
	@echo "  make clean   - remove build artifacts"

all: so

so:
	$(CXX) -fPIC -shared ipamirEvalMaxSAT2022glue.cc $(INCLUDES) \
		$(CADICAL_CPP) $(EVAL_CPP) $(MALIB_CPP) \
		$(LIBS) $(CXXFLAGS) -o $(OUT_SO)

static:
	$(CXX) -c ipamirEvalMaxSAT2022glue.cc $(INCLUDES) \
		lib/cadical/src/*.cpp lib/EvalMaxSAT/src/*.cpp lib/MaLib/src/*.cpp \
		$(LIBS) $(CXXFLAGS)
	$(AR) rvs $(OUT_A) *.o
	@rm -f *.o

clean:
	@rm -f $(OUT_SO) $(OUT_A) *.o
	@rm -f lib/EvalMaxSAT/src/*.gch lib/MaLib/src/*.gch 2> /dev/null || true
