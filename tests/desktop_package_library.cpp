// A real shared dependency exercises the package's DLL/.so placement and
// executable-relative loader paths, even though Kin itself links statically.
extern "C" const char* kin_package_fixture_marker() {
    return "Kin desktop package resources";
}
