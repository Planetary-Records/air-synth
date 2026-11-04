// Exercise the real receipt/exit policy without repeating the DSP workload.
#define main saturationMeasurementMain
#include "SaturationFeasibility.cpp"
#undef main
#include <type_traits>

template<class Finish> int finishExit(Finish finish)
{
    if constexpr(std::is_void_v<std::invoke_result_t<Finish>>) { finish();return 0; }
    else return finish();
}
int main(int argc,char** argv)
{
    if(argc!=3) return 2;
    const std::string outcome=argv[1];const fs::path directory=argv[2];
    if(!directory.is_absolute()||fs::exists(directory)) return 2;
    fs::create_directories(directory);Evidence evidence(directory,4,outcome=="cohort"?"alias-44100":"");
    output(directory/"frozen-manifest.json")<<"{\"fixture\":\"policy only, no DSP executed\"}\n";
    for(const char* name:{"analysis_calibration","neutral_response_valid","crossover_only_unity_0_1db",
        "combined_neutral_raw_input_envelope","neutral_wet_reference_null","two_identical_layer_sum",
        "mismatched_raw_dry_control","naive_uncompensated_split_control","compensation_all_219_integer_half_steps",
        "character_spectrum_valid","soft_symmetry","glow_asymmetry","glow_dc","dense_1e_attack_release",
        "dense_sag_range","dense_history_audio","dense_stereo_swap","operating_grid_finite",
        "alias_reference_qualification","alias_candidate_and_improvement","unoversampled_1x_negative_control",
        "mismatched_dry_reference_control"}) evidence.gates.push_back({name,true,false,"policy fixture"});
    if(outcome=="reject") evidence.gates.front().pass=false;
    else if(outcome=="invalid") evidence.gates.front().invalid=true;
    else if(outcome=="missing") evidence.gates.pop_back();
    else if(outcome=="duplicate") evidence.gates.back().name=evidence.gates.front().name;
    else if(outcome=="unknown") evidence.gates.back().name="unexpected_gate";
    else if(outcome=="artifact_error") fs::create_directory(directory/"artifact-hashes.csv");
    else if(outcome!="pass"&&outcome!="cohort") return 2;
    try { return finishExit([&] { return outcome=="cohort"?finishCohort(evidence):finish(evidence); }); }
    catch(const std::exception& error)
    {
        std::cerr<<"artifact-index I/O failure propagated from real finish: "<<error.what()<<'\n';
        return 2;
    }
}
