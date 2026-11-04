// Private runner policy entry; this target never dispatches a CPU measurement.
int saturationCpuPolicyEntry(const char* directory);
int main(int argc,char** argv)
{
    return argc==2?saturationCpuPolicyEntry(argv[1]):2;
}
