# vim: syntax=cmake
if(ENABLE_PRIMITIVES_VEC)
    add_definitions(-DENABLE_VECTOR_PRIMITIVES=1)
endif(ENABLE_PRIMITIVES_VEC)
if(ENABLE_PRIMITIVES_ASM)
    add_definitions(-DENABLE_ASM_PRIMITIVES=1)
endif(ENABLE_PRIMITIVES_ASM)

set(LIBCOMMON_HDR
    ../Lib/TLibCommon/CommonDef.h
    ../Lib/TLibCommon/ContextTables.h
    ../Lib/TLibCommon/NAL.h
    ../Lib/TLibCommon/SEI.h
    ../Lib/TLibCommon/TComBitCounter.h
    ../Lib/TLibCommon/TComBitStream.h
    ../Lib/TLibCommon/TComDataCU.h
    ../Lib/TLibCommon/TComLoopFilter.h
    ../Lib/TLibCommon/TComMotionInfo.h
    ../Lib/TLibCommon/TComPattern.h
    ../Lib/TLibCommon/TComPic.h
    ../Lib/TLibCommon/TComPicSym.h
    ../Lib/TLibCommon/TComPicYuv.h
    ../Lib/TLibCommon/TComPrediction.h
    ../Lib/TLibCommon/TComRdCost.h
    ../Lib/TLibCommon/TComRom.h
    ../Lib/TLibCommon/TComSampleAdaptiveOffset.h
    ../Lib/TLibCommon/TComSlice.h
    ../Lib/TLibCommon/TComTrQuant.h
    ../Lib/TLibCommon/TComWeightPrediction.h
    ../Lib/TLibCommon/TComYuv.h
    ../Lib/TLibCommon/TypeDef.h)
set(LIBCOMMON_SRC
    ../Lib/TLibCommon/TComBitStream.cpp
    ../Lib/TLibCommon/TComDataCU.cpp
    ../Lib/TLibCommon/TComLoopFilter.cpp
    ../Lib/TLibCommon/TComMotionInfo.cpp
    ../Lib/TLibCommon/TComPattern.cpp
    ../Lib/TLibCommon/TComPic.cpp
    ../Lib/TLibCommon/TComPicSym.cpp
    ../Lib/TLibCommon/TComPicYuv.cpp
    ../Lib/TLibCommon/TComPicYuvMD5.cpp
    ../Lib/TLibCommon/TComPrediction.cpp
    ../Lib/TLibCommon/TComRom.cpp
    ../Lib/TLibCommon/TComSampleAdaptiveOffset.cpp
    ../Lib/TLibCommon/TComSlice.cpp
    ../Lib/TLibCommon/TComTrQuant.cpp
    ../Lib/TLibCommon/TComWeightPrediction.cpp
    ../Lib/TLibCommon/TComYuv.cpp)
source_group(TLibCommon FILES ${LIBCOMMON_SRC})
source_group(TLibCommonH FILES ${LIBCOMMON_HDR})

if(GCC)
    set_source_files_properties(${LIBCOMMON_SRC} PROPERTIES COMPILE_FLAGS 
        "-Wno-sign-compare")
endif(GCC)
if(MSVC)
    # ignore these warnings from HM source
    # /wd4244 type conversion, possible loss of data
    # /wd4512 assignment operator could not be generated
    # /wd4127 conditional expression is constant
    # /wd4389 signed/unsigned mismatch
    # /wd4018 '<' signed/unsigned mismatch
    # /wd4800 performance warning: bool coersion
    set_source_files_properties(${LIBCOMMON_SRC} PROPERTIES COMPILE_FLAGS 
        "/wd4244 /wd4512 /wd4127 /wd4389 /wd4018 /wd4800")
    if (INTEL_CXX)
        add_definitions(/Qwd2557) # signed/unsigned mismatch
    endif()
endif(MSVC)

if(ENABLE_PRIMITIVES_VEC)
    if(HIGH_BIT_DEPTH)
        include_directories(../VectorClass)
    endif()
    set(SSE3  vec/pixel-sse3.cpp  vec/dct-sse3.cpp  vec/blockcopy-sse3.cpp)
    set(SSSE3 vec/pixel-ssse3.cpp vec/dct-ssse3.cpp vec/ipfilter-ssse3.cpp vec/intra-ssse3.cpp)
    set(SSE41 vec/pixel-sse41.cpp vec/dct-sse41.cpp vec/ipfilter-sse41.cpp vec/intra-sse41.cpp vec/pixel16-sse41.cpp)

    if (MSVC)
        add_definitions(/wd4127) # conditional expression is constant
        add_definitions(/wd4244) # 'argument' : conversion from 'int' to 'char', possible loss of data
        set(PRIMITIVES ${SSE3} ${SSSE3} ${SSE41})
        if (INTEL_CXX)
            add_definitions(/Qwd111) # statement is unreachable
            add_definitions(/Qwd128) # loop is unreachable
            add_definitions(/Qwd177) # declared function is unused
            add_definitions(/Qwd185) # dynamic initialization in unreachable code
            add_definitions(/Qwd280) # conditional expression is constant
        endif()
        if (NOT X64)
            # x64 implies SSE4, so this flag would have no effect (and it issues a warning)
            set_source_files_properties(${SSE3} ${SSSE3} ${SSE41} PROPERTIES COMPILE_FLAGS /arch:SSE2)
        endif()
    endif()
    if(GCC)
        if(CLANG)
            # llvm intrinsic headers trigger this warning
            add_definitions(-Wno-shadow)
            # llvm reports these warnings for the vector class headers
            add_definitions(-Wno-shift-overflow -Wno-uninitialized)
        endif()
        if(INTEL_CXX OR CLANG OR (NOT GCC_VERSION VERSION_LESS 4.3))
            set(PRIMITIVES ${SSE3} ${SSSE3} ${SSE41})
            set_source_files_properties(${SSE3}  PROPERTIES COMPILE_FLAGS "-msse3")
            set_source_files_properties(${SSSE3} PROPERTIES COMPILE_FLAGS "-mssse3")
            set_source_files_properties(${SSE41} PROPERTIES COMPILE_FLAGS "-msse4.1")
        endif()
    endif(GCC)

    set(VEC_PRIMITIVES vec/vec-primitives.cpp ${PRIMITIVES})
    source_group(Intrinsics FILES ${VEC_PRIMITIVES})
endif(ENABLE_PRIMITIVES_VEC)

if(ENABLE_PRIMITIVES_ASM)
    set(C_SRCS asm-primitives.cpp pixel.h mc.h ipfilter8.h blockcopy8.h)
    set(A_SRCS pixel-a.asm const-a.asm cpu-a.asm sad-a.asm mc-a.asm mc-a2.asm ipfilter8.asm pixel-util.asm blockcopy8.asm)
    if (NOT X64)
        set(A_SRCS ${A_SRCS} pixel-32.asm)
    endif()

    if(MSVC_IDE)
        # MSVC requires custom build rules in the main cmake script for yasm
        set(MSVC_ASMS "${A_SRCS}" CACHE INTERNAL "yasm sources")
        set(A_SRCS)
    else()
        enable_language(ASM_YASM)
    endif()

    foreach(SRC ${A_SRCS} ${C_SRCS})
        set(ASM_PRIMITIVES ${ASM_PRIMITIVES} x86/${SRC})
    endforeach()
endif(ENABLE_PRIMITIVES_ASM)

if(GCC)
    if (NOT X64)
        # force gcc to generate code for sync primitives
        set_source_files_properties(threadpool.cpp reference.cpp wavefront.cpp common.cpp PROPERTIES COMPILE_FLAGS -march=i686)
    endif()
    if (NOT CLANG AND NOT INTEL_CXX)
        set_source_files_properties(cpu.cpp PROPERTIES COMPILE_FLAGS -Wno-narrowing)
    endif()
endif(GCC)
set_source_files_properties(version.cpp PROPERTIES COMPILE_FLAGS -DX265_VERSION=${X265_VERSION})

add_library(common OBJECT
    ${LIBCOMMON_SRC} ${LIBCOMMON_HDR}
    ${ASM_PRIMITIVES} ${VEC_PRIMITIVES}
    primitives.cpp primitives.h
    pixel.cpp dct.cpp ipfilter.cpp intrapred.cpp
    cpu.cpp cpu.h version.cpp
    threading.cpp threading.h
    threadpool.cpp threadpool.h
    wavefront.h wavefront.cpp
    md5.cpp md5.h
    TShortYUV.cpp TShortYUV.h mv.h
    common.cpp common.h
    lowres.cpp lowres.h
    piclist.cpp piclist.h)
