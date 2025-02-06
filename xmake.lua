add_rules("mode.debug", "mode.release")

add_requires("openssl3", "cppcodec", "yaml-cpp")

target("qmi_sms_reader")
    set_kind("binary")
    -- add_files("src/*.cpp")
    add_files("src/sms.cpp")
    add_files("src/SmsReader/*.cpp")
    add_includedirs("src/SmsReader")
    add_files("src/SignUtils/*.cpp")
    add_includedirs("src/SignUtils")

    set_languages("c++20")

    add_includedirs("PDUlib/src")
    add_defines("DESKTOP_PDU")
    add_files("PDUlib/src/*.cpp")

    add_packages("openssl3", "cppcodec", "yaml-cpp")

    add_packages("pkgconfig::glib-2.0", "pkgconfig::qmi-glib")
    add_links("gio-2.0", "gobject-2.0", "glib-2.0")

    add_ldflags("-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic -lc -Wl,-Bdynamic")
    add_links("qmi-glib", "gio-2.0", "gobject-2.0", "glib-2.0")

    -- 补全一些 clangd 找不到的库
    add_includedirs(
        "/usr/include/glib-2.0",
        "/usr/lib/aarch64-linux-gnu/glib-2.0/include",
        "/usr/include/libqrtr-glib",
        "/usr/local/include/libqmi-glib")

    -- 指定 libqmi 的库目录
    add_linkdirs("/usr/lib")

target("qmi_sms_reader_musl")
    local staging_dir = os.getenv("STAGING_DIR")
    if not staging_dir then
        staging_dir = ""
    end
    set_kind("binary")
    -- add_files("src/*.cpp")
    add_files("src/sms.cpp")
    add_files("src/SmsReader/*.cpp")
    add_includedirs("src/SmsReader")
    add_files("src/SignUtils/*.cpp")
    add_includedirs("src/SignUtils")

    set_languages("c++20")

    add_packages("openssl3", "cppcodec", "yaml-cpp")

    add_includedirs("PDUlib/src")
    add_defines("DESKTOP_PDU")
    add_files("PDUlib/src/*.cpp")

    add_packages("pkgconfig::glib-2.0", "pkgconfig::qmi-glib")
    add_links("gio-2.0", "gobject-2.0", "glib-2.0", "qmi-glib")
    
    add_linkdirs(
        staging_dir .. "/target-aarch64_generic_musl/usr/lib",
        staging_dir .. "/target-aarch64_generic_musl/root-rockchip/usr/lib")
    
    -- 如果需要静态链接其他库，可选择去掉或注释下面这行
    -- add_ldflags("-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic -lc -Wl,-Bdynamic")
    
    -- 显式链接 libgcc_s 以解决 _Unwind_Resume 问题
    add_ldflags("-lgcc_s", {force = true})
    
    -- 添加 rpath-link 选项帮助链接器找到依赖的动态库
    add_ldflags("-Wl,-rpath-link," .. staging_dir .. "/target-aarch64_generic_musl/usr/lib",
                "-Wl,-rpath-link," .. staging_dir .. "/target-aarch64_generic_musl/root-rockchip/usr/lib", 
                {force = true})
    
    add_includedirs(
        staging_dir .. "/target-aarch64_generic_musl/usr/include", 
        staging_dir .. "/target-aarch64_generic_musl/usr/include/glib-2.0",
        staging_dir .. "/target-aarch64_generic_musl/usr/include/libqmi-glib",
        staging_dir .. "/target-aarch64_generic_musl/usr/include/libqrtr-glib")
