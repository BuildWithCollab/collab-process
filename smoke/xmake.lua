set_languages("c++23")

add_repositories("BuildWithCollab https://github.com/BuildWithCollab/Packages.git")
add_requires("collab-process")

target("smoke-test")
    set_kind("binary")
    add_files("tests.cpp")
    add_packages("collab-process")
