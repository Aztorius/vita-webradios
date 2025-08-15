cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()


project(webradio)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Webradio")
set(VITA_TITLEID  "WEBRADIOS")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/audio/mp3.c
  src/m3u_parser/m3u.c
  src/gui/imgui_demo.cpp
)

target_link_libraries(${PROJECT_NAME}
  imgui
  vitaGL
  vitashark
  SceCommonDialog_stub
  SceGxm_stub
  SceSysmodule_stub
  SceTouch_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceKernelDmacMgr_stub
  SceShaccCgExt
  SceShaccCg_stub
  taihen_stub
  SceSsl_stub
  SceHttp_stub
  SceNetCtl_stub
  SceNet_stub
  SceDisplay_stub
  SceCtrl_stub
  SceAudio_stub
  SceAudiodec_stub
  ScePaf_stub
  SceSysmem_stub
  SceLibKernel_stub
  mpg123
  mathneon
  m
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE default_playlist.m3u default_playlist.m3u
)
