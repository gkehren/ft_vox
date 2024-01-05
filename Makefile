
# ------------------
CXX = g++
CXXFLAGS = -std=c++17
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lGL -lglfw -lfreetype
	INCDIR = -I src -I/usr/include/freetype2
endif
ifeq ($(UNAME_S),Darwin)    # Darwin is for MacOS
    LDFLAGS = -framework OpenGL -lglfw -lfreetype -L/opt/homebrew/lib
	INCDIR = -I src -I /opt/homebrew/include/ -I /opt/homebrew/include/freetype2
endif
# ==================

# ----- Paths -----
SRCDIR = src
OBJDIR = obj
# ==================

# ----- Colors -----
BLACK:="\033[1;30m"
RED:="\033[1;31m"
GREEN:="\033[1;32m"
CYAN:="\033[1;35m"
PURPLE:="\033[1;36m"
WHITE:="\033[1;37m"
EOC:="\033[0;0m"
# ==================

# ------ Auto ------
SRC = $(wildcard $(SRCDIR)/**/*.cpp $(SRCDIR)/*.cpp)
OBJ = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRC))
GLAD_SRC = $(SRCDIR)/glad/glad.c
# ==================

TARGET = ft_vox

all: ${TARGET}

${TARGET}: ${OBJ}
	@echo ${CYAN} " - Compiling $@" $(RED)
	@${CXX} -o $@ $^ ${GLAD_SRC} ${LDFLAGS} ${INCDIR}
	@echo $(GREEN) " - OK" $(EOC)

${OBJDIR}/%.o: ${SRCDIR}/%.cpp
	@mkdir -p $(@D)
	@echo ${PURPLE} " - Compiling $< into $@" ${EOC}
	@${CXX} ${CXXFLAGS} ${INCDIR} -c -o $@ $<

%.cpp:
	@echo ${RED}"Missing file : $@" ${EOC}

clean:
	@rm -rf ${OBJDIR}

fclean:	clean
	@rm -f ${TARGET}

re:	fclean
	@${MAKE} all

.PHONY:	all clean fclean re
