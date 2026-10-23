extern "C" int kin_release_marker(void);
int main() { return kin_release_marker() == 42 ? 0 : 1; }
