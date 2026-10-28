extern "C" __declspec(dllimport) int blendnet_runtime_fixture();
int main() { return blendnet_runtime_fixture() == 42 ? 0 : 1; }
