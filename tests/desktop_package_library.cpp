// A real shared dependency exercises the package's DLL/.so placement and
// executable-relative loader paths, even though Kin itself links statically.
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
const char* kin_package_fixture_marker() {
    return "Kin desktop package resources";
}
