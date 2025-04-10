add_rules("mode.debug", "mode.release")

add_requires("ixwebsocket", {configs = {use_tls = true, ssl = "mbedtls"}})
add_requires("yaml-cpp", {configs = {shared = false}})
add_requires("glog", {configs = {shared = false}})
add_requires("glib-2.0", {system = true})
add_requires("qmi-glib", {system = true})
add_requires("cppcodec", "nlohmann_json")

add_repositories("local-repo build")
add_requires("ixwebsocket-custom", {configs = {use_tls = true, ssl = "mbedtls"}})

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

    add_packages("openssl", "cppcodec", "yaml-cpp", "ixwebsocket-custom", "nlohmann_json", "glog", "glib-2.0", "qmi-glib")
    add_links("gio-2.0", "gobject-2.0", "glib-2.0")

    add_ldflags("-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic -lc -Wl,-Bdynamic")
    add_links("qmi-glib", "gio-2.0", "gobject-2.0", "glib-2.0")

    -- 指定 libqmi 的库目录
    add_linkdirs("/usr/lib")

target("qmi_sms_reader_musl")
    local staging_dir = os.getenv("STAGING_DIR")
        if not staging_dir then
        if is_plat("cross") then
            print("Please set STAGING_DIR environment variable")
        else
            staging_dir = ""
        end
    end
    set_kind("binary")
    -- add_files("src/*.cpp")
    add_files("src/sms.cpp")
    add_files("src/SmsReader/*.cpp")
    add_includedirs("src/SmsReader")
    add_files("src/SignUtils/*.cpp")
    add_includedirs("src/SignUtils")

    set_languages("c++20")

    add_packages("openssl", "cppcodec", "yaml-cpp", "ixwebsocket-custom", "nlohmann_json", "glog")

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
