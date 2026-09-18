// Which OpenXR runtime would CS2 actually get, and does XR_RUNTIME_JSON change it?
//
// Small enough to trust, and it answers a question that otherwise costs a game launch and
// a headset on someone's head. It opens the loader the same way the hook does -- by
// explicit path, never linked -- creates an instance, and prints what answered.
//
// Creating an instance needs no headset. Only xrGetSystem does, so this runs with
// everything switched off and still tells you which runtime is in front of you.
//
//   xr-probe.exe [path\to\openxr_loader.dll]
//
// Set XR_RUNTIME_JSON before running it to select a runtime for this process alone:
//
//   $env:XR_RUNTIME_JSON = 'C:\Program Files\Meta Horizon\Support\oculus-runtime\oculus_openxr_64.json'
//   xr-probe.exe

#include <windows.h>
#include <stdio.h>

#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static const wchar_t * const kDefaultLoader =
    L"D:\\Dev\\cs2-vr-tools\\openxr\\pkg\\native\\x64\\release\\bin\\openxr_loader.dll";

int main(int argc, char ** argv) {
    wchar_t loaderPath[MAX_PATH];
    if (argc > 1) {
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, loaderPath, MAX_PATH);
    } else {
        wcscpy_s(loaderPath, kDefaultLoader);
    }

    char * runtimeJson = nullptr;
    size_t runtimeJsonLength = 0;
    _dupenv_s(&runtimeJson, &runtimeJsonLength, "XR_RUNTIME_JSON");
    printf("XR_RUNTIME_JSON : %s\n", runtimeJson ? runtimeJson : "(not set - the registry decides)");
    printf("loader          : %ls\n", loaderPath);

    HMODULE loader = LoadLibraryW(loaderPath);
    if (!loader) {
        printf("FAILED: could not load the loader (error %lu)\n", GetLastError());
        return 1;
    }

    auto getProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    if (!getProc) {
        printf("FAILED: the loader has no xrGetInstanceProcAddr\n");
        return 1;
    }

    PFN_xrCreateInstance createInstance = nullptr;
    getProc(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance);
    if (!createInstance) {
        printf("FAILED: no xrCreateInstance\n");
        return 1;
    }

    // The hook asks for the D3D11 extension, so ask for it too -- a runtime that cannot
    // provide it is no use to this project and should fail here rather than later.
    //
    // Spelled out rather than using XR_KHR_D3D11_ENABLE_EXTENSION_NAME, because that macro
    // only exists once XR_USE_GRAPHICS_API_D3D11 is defined, which drags in d3d11.h for a
    // program that never touches a device.
    const char * extensions[] = { "XR_KHR_D3D11_enable" };

    XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(info.applicationInfo.applicationName, "cs2-vr-spectator xr-probe");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = createInstance(&info, &instance);
    if (XR_FAILED(r)) {
        printf("FAILED: xrCreateInstance returned %d\n", (int)r);
        printf("        -2 is XR_ERROR_RUNTIME_FAILURE, -3 XR_ERROR_RUNTIME_UNAVAILABLE.\n");
        return 1;
    }

    PFN_xrGetInstanceProperties getProperties = nullptr;
    getProc(instance, "xrGetInstanceProperties", (PFN_xrVoidFunction*)&getProperties);

    XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
    if (getProperties && XR_SUCCEEDED(getProperties(instance, &properties))) {
        printf("runtime         : %s %u.%u.%u\n",
            properties.runtimeName,
            (unsigned)XR_VERSION_MAJOR(properties.runtimeVersion),
            (unsigned)XR_VERSION_MINOR(properties.runtimeVersion),
            (unsigned)XR_VERSION_PATCH(properties.runtimeVersion));
    }

    // Now the part that needs hardware. Reported rather than treated as a failure: no
    // headset is the normal state of this machine most of the time.
    PFN_xrGetSystem getSystem = nullptr;
    getProc(instance, "xrGetSystem", (PFN_xrVoidFunction*)&getSystem);

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrResult sr = getSystem ? getSystem(instance, &systemInfo, &systemId) : XR_ERROR_FUNCTION_UNSUPPORTED;

    if (XR_SUCCEEDED(sr)) {
        PFN_xrGetSystemProperties getSystemProperties = nullptr;
        getProc(instance, "xrGetSystemProperties", (PFN_xrVoidFunction*)&getSystemProperties);
        XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
        if (getSystemProperties && XR_SUCCEEDED(getSystemProperties(instance, systemId, &systemProperties))) {
            printf("headset         : %s, up to %ux%u per eye\n",
                systemProperties.systemName,
                systemProperties.graphicsProperties.maxSwapchainImageWidth,
                systemProperties.graphicsProperties.maxSwapchainImageHeight);
        } else {
            printf("headset         : present\n");
        }
    } else if (XR_ERROR_FORM_FACTOR_UNAVAILABLE == sr) {
        printf("headset         : none connected (XR_ERROR_FORM_FACTOR_UNAVAILABLE)\n");
        printf("                  The runtime is up; there is simply no headset in front of it.\n");
    } else {
        printf("headset         : xrGetSystem returned %d\n", (int)sr);
    }

    PFN_xrDestroyInstance destroyInstance = nullptr;
    getProc(instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&destroyInstance);
    if (destroyInstance) destroyInstance(instance);

    if (runtimeJson) free(runtimeJson);
    return 0;
}
