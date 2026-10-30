cmake_minimum_required(VERSION 3.16)
project(shizuku_shell LANGUAGES CXX)
enable_language(RC) # MinGW 下编译 .rc 需要 windres 语言支持

# MinGW 的 windres 会 fork gcc 做预处理。若 gcc 不在**系统** PATH 里
# （Git Bash 里 export 的 PATH 传不到它），就会报一句 "preprocessing failed" 掩盖真相。
# 这里显式绑死预处理器，构建结果不依赖调用者的环境。
if(MINGW)
    get_filename_component(_mingw_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --preprocessor=\"${_mingw_dir}/gcc.exe\"")
endif()

# RC 图标路径：生成到构建目录，避免 windres 在 build/ 下解析不到相对路径
configure_file(res/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

# ── 后端内核：工程解析 + LUME 渲染 + 编辑命令（只依赖 Qt Gui，可脱离界面被复用）
add_library(sfcore STATIC
    src/core/lume.cpp
    src/core/sf_project.cpp
    src/core/sf_render.cpp
    src/core/sf_edit.cpp
    src/core/sf_presets.cpp
    src/core/sf_ass.cpp
    src/core/sf_audio.cpp
    src/core/sf_lrc.cpp
    src/core/sf_lint.cpp
)
target_include_directories(sfcore PUBLIC src)
target_link_libraries(sfcore PUBLIC Qt6::Gui)

# ── 无头渲染工具：把任意一帧导出成 PNG（渲染管线自检 + 与前端对拍）
add_executable(sf_dump tools/sf_dump.cpp)
target_link_libraries(sf_dump PRIVATE sfcore)

# ── 编辑命令层单元测试（无界面，退出码 0/1）
add_executable(sf_edit_test tools/sf_edit_test.cpp)
target_link_libraries(sf_edit_test PRIVATE sfcore Qt6::Gui)

# 无头测试要能直接命令行跑（不依赖 build.sh 里 export 的 PATH）：
# 把 Qt / MinGW 的运行时 DLL 拷到产物目录；找不到就跳过，不影响构建。
get_filename_component(_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
foreach(_runtime Qt6Core Qt6Gui libstdc++-6 libgcc_s_seh-1 libwinpthread-1)
    foreach(_dir "${_qt_bin}" "${_mingw_bin}")
        if(EXISTS "${_dir}/${_runtime}.dll")
            add_custom_command(TARGET sf_edit_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_dir}/${_runtime}.dll" "$<TARGET_FILE_DIR:sf_edit_test>")
            break()
        endif()
    endforeach()
endforeach()

# ── Qt 界面壳
add_executable(shizuku_shell
    src/main.cpp
    src/theme.cpp
    src/Splash.cpp
    src/MainWindow.cpp
    src/PreviewPane.cpp
    src/TimelinePane.cpp
    res/splash.qrc
    # 注意：EXE 图标（app.rc）暂不参与构建。
    # MinGW 的 windres 要 fork gcc 预处理，而 Git Bash 里 export 的 PATH
    # 传不进它的子进程，全量构建会报 "preprocessing failed"（增量构建因旧 .obj
    # 存在而掩盖，属真 bug 而非环境偶发）。图标不影响功能，待单独修。
    # ${CMAKE_CURRENT_BINARY_DIR}/app.rc
)

target_link_libraries(shizuku_shell PRIVATE Qt6::Widgets sfcore)

# 让 exe 不依赖环境变量就能找到 Qt 平台插件。
# 缺了它双击会直接退出，只在 stderr 留一句
# "Could not find the Qt platform plugin windows" —— 看起来像程序坏了。
get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf"
     "[Paths]\nPrefix=${_qt_prefix}\nPlugins=plugins\n")
