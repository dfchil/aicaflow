ENJ_INJECT_QFONT:=1
ENJ_ROMDIR:=embeds
ENJ_INCLUDES+=-I$(realpath ${ENJ_BASEDIR}../../include) -I$(realpath ${ENJ_BASEDIR}/include)
ENJ_LDLIBS+=-L $(realpath ${ENJ_BASEDIR}../../lib) -lafx
