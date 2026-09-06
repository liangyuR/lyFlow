# LyFlow 标准算子包：点云域的 14 个算子（ADR-0014）。
# 本文件同时是全仓库唯一一处 find_package(PCL)：别的包链 lyflow_pcl_support 就好。
# LYFLOW_PACK_DIR 由 core/CMakeLists.txt 在 include 之前设好。

set(LYFLOW_PCL_COMPONENTS common io filters kdtree search segmentation sample_consensus features)
find_package(PCL 1.12 REQUIRED COMPONENTS ${LYFLOW_PCL_COMPONENTS})

# PCL::common 与 pcl_common 两种目标名都认，都没有时退回 ${PCL_LIBRARIES}。
# 为什么不能硬写一种见 core/README.md「构建」。
set(LYFLOW_PCL_LIBS "")
foreach(comp IN LISTS LYFLOW_PCL_COMPONENTS)
  if(TARGET PCL::${comp})
    list(APPEND LYFLOW_PCL_LIBS PCL::${comp})
  elseif(TARGET pcl_${comp})
    list(APPEND LYFLOW_PCL_LIBS pcl_${comp})
  endif()
endforeach()
message(STATUS "lyflow: PCL targets = ${LYFLOW_PCL_LIBS}")

# S3：需要 PCL 的包链这一个目标就够了 —— include 目录、库、告警屏蔽一次配齐。
add_library(lyflow_pcl_support INTERFACE)
target_include_directories(lyflow_pcl_support INTERFACE "${LYFLOW_PACK_DIR}/include")
if(LYFLOW_PCL_LIBS)
  target_link_libraries(lyflow_pcl_support INTERFACE ${LYFLOW_PCL_LIBS})
else()
  target_link_libraries(lyflow_pcl_support INTERFACE ${PCL_LIBRARIES})
  target_include_directories(lyflow_pcl_support INTERFACE ${PCL_INCLUDE_DIRS})
  target_compile_definitions(lyflow_pcl_support INTERFACE ${PCL_DEFINITIONS})
endif()
if(MSVC)
  # PCL/Eigen 自己的告警不是我们能修的，但包自己的代码仍然 /W4
  target_compile_options(lyflow_pcl_support INTERFACE /wd4127 /wd4267 /wd4244 /wd4324)
endif()

file(GLOB STD_PC_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_PACK_DIR}/ops/*.cpp" "${LYFLOW_PACK_DIR}/src/*.cpp")
file(GLOB STD_PC_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         std-pointcloud
  VERSION      0.1.0
  SOURCES      ${STD_PC_SOURCES}
  TEST_SOURCES ${STD_PC_TESTS}
  PCH          "${LYFLOW_PACK_DIR}/include/lyflow_pcl/pcl_pch.h"
  LINK         lyflow_pcl_support
)
