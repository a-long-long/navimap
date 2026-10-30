cmake_minimum_required(VERSION 3.16)
project(tut12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_PREFIX_PATH "D:/Qt/6.11.2/mingw_64" CACHE PATH "Qt6 安装前缀")

find_package(Qt6 REQUIRED COMPONENTS Gui Widgets)

# 真实 GUI 版（显示窗口）用这个 main_windows.cpp；本课验证用 main.cpp（只 QImage 存 PNG，无需事件循环）
add_executable(tut12 main.cpp)
target_link_libraries(tut12 Qt6::Gui)

# 若改用窗口版：换成下面这行（需要 Qt6::Widgets 与 moc）
# add_executable(tut12_win main_windows.cpp)
# target_link_libraries(tut12_win Qt6::Widgets)
