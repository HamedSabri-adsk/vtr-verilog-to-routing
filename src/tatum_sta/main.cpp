#include <ctime>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>

#include <valgrind/callgrind.h>

#include "tatum_assert.hpp"

#include "timing_analyzer_interfaces.hpp"
#include "full_timing_analyzers.hpp"
#include "graph_walkers.hpp"

#include "TimingGraph.hpp"
#include "TimingConstraints.hpp"

#include "FixedDelayCalculator.hpp"
#include "ConstantDelayCalculator.hpp"

#include "vpr_timing_graph_common.hpp"

#include "sta_util.hpp"
#include "verify.hpp"

#define NUM_SERIAL_RUNS 1
#define NUM_PARALLEL_RUNS (1*NUM_SERIAL_RUNS)
//#define NUM_SERIAL_RUNS 20
//#define NUM_PARALLEL_RUNS (3*NUM_SERIAL_RUNS)
#define OPTIMIZE_NODE_EDGE_ORDER

//Do we perform verification checks?
#define TATUM_TATUM_ASSERT_VPR_TO_TATUM

//Currently don't check for differences in the other direction (from us to VPR),
//since we do a single traversal we generate extra ancillary timing tags which
//will not match VPR
//#define CHECK_TATUM_TO_VPR_DIFFERENCES

typedef std::chrono::duration<double> dsec;
typedef std::chrono::high_resolution_clock Clock;

using std::cout;
using std::endl;

int main(int argc, char** argv) {
    if(argc != 2) {
        cout << "Usage: " << argv[0] << " tg_echo_file" << endl;
        return 1;
    }

    struct timespec prog_start, load_start, node_reorder_start, edge_reorder_start, verify_start, reset_start;
    struct timespec prog_end, load_end, node_reorder_end, edge_reorder_end, verify_end, reset_end;

    clock_gettime(CLOCK_MONOTONIC, &prog_start);

    cout << "Time class sizeof  = " << sizeof(Time) << " bytes. Time Vec Width: " << TIME_VEC_WIDTH << endl;
    cout << "Time class alignof = " << alignof(Time) << endl;

    cout << "TimingTag class sizeof  = " << sizeof(TimingTag) << " bytes." << endl;
    cout << "TimingTag class alignof = " << alignof(TimingTag) << " bytes." << endl;

    cout << "TimingTags class sizeof  = " << sizeof(TimingTags) << " bytes." << endl;
    cout << "TimingTags class alignof = " << alignof(TimingTags) << " bytes." << endl;

    //Raw outputs of parser
    auto timing_graph = std::make_shared<TimingGraph>();
    auto timing_constraints = std::make_shared<TimingConstraints>();
    VprArrReqTimes orig_expected_arr_req_times;
    std::vector<float> orig_edge_delays;
    std::vector<BlockId> node_logical_blocks;

    //Potentially modified based on parser output
    VprArrReqTimes expected_arr_req_times;
    std::vector<float> edge_delays;

    std::set<NodeId> const_gen_fanout_nodes;
    std::set<NodeId> clock_gen_fanout_nodes;

    {
        clock_gettime(CLOCK_MONOTONIC, &load_start);

        yyin = fopen(argv[1], "r");
        if(yyin != NULL) {
            int error = yyparse(*timing_graph, orig_expected_arr_req_times, *timing_constraints, node_logical_blocks, orig_edge_delays);
            if(error) {
                cout << "Parse Error" << endl;
                fclose(yyin);
                return 1;
            }
            fclose(yyin);
        } else {
            cout << "Could not open file " << argv[1] << endl;
            return 1;
        }

        //Fix up the timing graph.
        //VPR doesn't have edges from FF_CLOCKs to FF_SOURCEs and FF_SINKs,
        //but we require them. So post-process the timing graph here to add them.
        add_ff_clock_to_source_sink_edges(*timing_graph, node_logical_blocks, orig_edge_delays);
        //We then need to re-levelize the graph
        timing_graph->levelize();

        cout << "Timing Graph Stats:" << endl;
        cout << "  Nodes : " << timing_graph->nodes().size() << endl;
        cout << "  Levels: " << timing_graph->levels().size() << endl;
        cout << "Num Clocks: " << orig_expected_arr_req_times.get_num_clocks() << endl;

        cout << endl;

#ifdef OPTIMIZE_NODE_EDGE_ORDER
        clock_gettime(CLOCK_MONOTONIC, &edge_reorder_start);

        //Re-order edges
        cout << "Re-allocating edges so levels are in contiguous memory";
        tatum::linear_map<EdgeId,EdgeId> vpr_edge_map = timing_graph->optimize_edge_layout();

        clock_gettime(CLOCK_MONOTONIC, &edge_reorder_end);
        cout << " (took " << time_sec(edge_reorder_start, edge_reorder_end) << " sec)" << endl;


        //Adjust the edge delays to reflect the new ordering
        edge_delays = std::vector<float>(orig_edge_delays.size());
        for(size_t i = 0; i < orig_edge_delays.size(); i++) {
            EdgeId new_id = vpr_edge_map[EdgeId(i)];
            edge_delays[size_t(new_id)] = orig_edge_delays[i];
        }

        clock_gettime(CLOCK_MONOTONIC, &node_reorder_start);

        //Re-order nodes
        cout << "Re-allocating nodes so levels are in contiguous memory";
        tatum::linear_map<NodeId,NodeId> vpr_node_map = timing_graph->optimize_node_layout();

        clock_gettime(CLOCK_MONOTONIC, &node_reorder_end);
        cout << " (took " << time_sec(node_reorder_start, node_reorder_end) << " sec)" << endl;

        //Re-build the expected_arr_req_times to reflect the new node orderings
        expected_arr_req_times = VprArrReqTimes();
        expected_arr_req_times.set_num_nodes(orig_expected_arr_req_times.get_num_nodes());

        //Collect the clock domains with actual values (clock domain IDs could be discontinous))))

        for(auto src_domain : orig_expected_arr_req_times.domains()) {
            //For every clock domain pair
            for(int i = 0; i < orig_expected_arr_req_times.get_num_nodes(); i++) {
                NodeId new_id = vpr_node_map[NodeId(i)];
                expected_arr_req_times.add_arr_time(src_domain, new_id, orig_expected_arr_req_times.get_arr_time(src_domain, NodeId(i)));
                expected_arr_req_times.add_req_time(src_domain, new_id, orig_expected_arr_req_times.get_req_time(src_domain, NodeId(i)));
            }
        }

        //Adjust the timing constraints
        timing_constraints->remap_nodes(vpr_node_map);

#else
        expected_arr_req_times = orig_expected_arr_req_times;
        edge_delays = orig_edge_delays;
#endif

        clock_gettime(CLOCK_MONOTONIC, &load_end);

        const_gen_fanout_nodes = identify_constant_gen_fanout(*timing_graph);
        clock_gen_fanout_nodes = identify_clock_gen_fanout(*timing_graph);
    }
    cout << "Loading took: " << time_sec(load_start, load_end) << " sec" << endl;
    cout << endl;

    /*
     *timing_constraints.print();
     */

    int n_histo_bins = 10;
    print_level_histogram(*timing_graph, n_histo_bins);
    print_node_fanin_histogram(*timing_graph, n_histo_bins);
    print_node_fanout_histogram(*timing_graph, n_histo_bins);
    cout << endl;

    /*
     *cout << "Timing Graph" << endl;
     *print_timing_graph(timing_graph);
     *cout << endl;
     */

    /*
     *cout << "Levelization" << endl;
     *print_levelization(timing_graph);
     *cout << endl;
     */


    //Create the delay calculator
    auto delay_calculator = std::make_shared<FixedDelayCalculator>(edge_delays);

    //Create the timing analyzer
    std::shared_ptr<SetupTimingAnalyzer> serial_analyzer = std::make_shared<SetupFullTimingAnalyzer<SerialWalker, FixedDelayCalculator>>(timing_graph, timing_constraints, delay_calculator);

    //Performance variables
    float serial_verify_time = 0.;
    float serial_reset_time = 0.;
    size_t serial_arr_req_verified = 0;
    std::map<std::string,float> serial_prof_data;
    {
        cout << "Running Serial Analysis " << NUM_SERIAL_RUNS << " times" << endl;

        //To selectively profile using callgrind:
        //  valgrind --tool=callgrind --collect-atstart=no --instr-atstart=no --cache-sim=yes --cacheuse=yes ./command
        CALLGRIND_START_INSTRUMENTATION;
        for(int i = 0; i < NUM_SERIAL_RUNS; i++) {
            //Analyze

            {
                auto start = Clock::now();

                CALLGRIND_TOGGLE_COLLECT;
                serial_analyzer->update_timing();
                CALLGRIND_TOGGLE_COLLECT;

                serial_prof_data["analysis_sec"] += std::chrono::duration_cast<dsec>(Clock::now() - start).count();
            }

            for(auto key : {"arrival_pre_traversal_sec", "arrival_traversal_sec", "required_pre_traversal_sec", "required_traversal_sec"}) {
                serial_prof_data[key] += serial_analyzer->get_profiling_data(key);
            }

            cout << ".";
            cout.flush();

            //print_setup_tags(timing_graph, serial_analyzer);
            //print_hold_tags(timing_graph, serial_analyzer);

            //Verify
            clock_gettime(CLOCK_MONOTONIC, &verify_start);

#ifdef TATUM_TATUM_ASSERT_VPR_TO_TATUM
            if(i == 0 || i == NUM_SERIAL_RUNS - 1) {
                serial_arr_req_verified = verify_analyzer(*timing_graph, serial_analyzer,
                                                          expected_arr_req_times, const_gen_fanout_nodes,
                                                          clock_gen_fanout_nodes );
            }
#endif

            clock_gettime(CLOCK_MONOTONIC, &verify_end);
            serial_verify_time += time_sec(verify_start, verify_end);

            if(i < NUM_SERIAL_RUNS-1) {
                clock_gettime(CLOCK_MONOTONIC, &reset_start);
                serial_analyzer->reset_timing();
                clock_gettime(CLOCK_MONOTONIC, &reset_end);
                serial_reset_time += time_sec(reset_start, reset_end);
            }
        }
        CALLGRIND_STOP_INSTRUMENTATION;

        for(auto kv : serial_prof_data) {
            serial_prof_data[kv.first] /= NUM_SERIAL_RUNS;
        }

        cout << endl;
        cout << "Serial Analysis took " << std::setprecision(6) << std::setw(6) << serial_prof_data["analysis_sec"]*NUM_SERIAL_RUNS << " sec, AVG: " << serial_prof_data["analysis_sec"] << " s" << endl;

        cout << "\tArr Pre-traversal Avg: " << std::setprecision(6) << std::setw(6) << serial_prof_data["arrival_pre_traversal_sec"] << " s";
        cout << " (" << std::setprecision(2) << serial_prof_data["arrival_pre_traversal_sec"]/serial_prof_data["analysis_sec"] << ")" << endl;

        cout << "\tReq Pre-traversal Avg: " << std::setprecision(6) << std::setw(6) << serial_prof_data["required_pre_traversal_sec"] << " s";
        cout << " (" << std::setprecision(2) << serial_prof_data["required_pre_traversal_sec"]/serial_prof_data["analysis_sec"] << ")" << endl;

        cout << "\tArr     traversal Avg: " << std::setprecision(6) << std::setw(6) << serial_prof_data["arrival_traversal_sec"]<< " s";
        cout << " (" << std::setprecision(2) << serial_prof_data["arrival_traversal_sec"]/serial_prof_data["analysis_sec"] << ")" << endl;

        cout << "\tReq     traversal Avg: " << std::setprecision(6) << std::setw(6) << serial_prof_data["required_traversal_sec"] << " s";
        cout << " (" << std::setprecision(2) << serial_prof_data["required_traversal_sec"]/serial_prof_data["analysis_sec"] << ")" << endl;

        cout << "Verifying Serial Analysis took: " << serial_verify_time << " sec" << endl;
        if(serial_arr_req_verified != 2 * timing_graph->nodes().size() * expected_arr_req_times.get_num_clocks()) { //2x for arr and req
            cout << "WARNING: Expected arr/req times differ from number of nodes. Verification may not have occured!" << endl;
        } else {
            cout << "\tVerified " << serial_arr_req_verified << " arr/req times accross " << timing_graph->nodes().size() << " nodes and " << expected_arr_req_times.get_num_clocks() << " clocks" << endl;
        }
        cout << "Resetting Serial Analysis took: " << serial_reset_time << " sec" << endl;
        cout << endl;
    }

    if(timing_graph->nodes().size() < 1000) {
        cout << "Writing Anotated Timing Graph Dot File" << endl;
        std::ofstream tg_setup_dot_file("tg_setup_annotated.dot");
        write_dot_file_setup(tg_setup_dot_file, *timing_graph, serial_analyzer, delay_calculator);

        //std::ofstream tg_hold_dot_file("tg_hold_annotated.dot");
        //write_dot_file_hold(tg_hold_dot_file, *timing_graph, serial_analyzer, delay_calculator);
    } else {
        cout << "Skipping writting dot file due to large graph size" << endl;
    }
    cout << endl;

#if NUM_PARALLEL_RUNS > 0
    std::shared_ptr<SetupTimingAnalyzer> parallel_analyzer = std::make_shared<SetupFullTimingAnalyzer<ParallelLevelizedCilkWalker, FixedDelayCalculator>>(timing_graph, timing_constraints, delay_calculator);

    //float parallel_analysis_time = 0;
    //float parallel_pretraverse_time = 0.;
    //float parallel_fwdtraverse_time = 0.;
    //float parallel_bcktraverse_time = 0.;
    //float parallel_analysis_time_avg = 0;
    //float parallel_pretraverse_time_avg = 0.;
    //float parallel_fwdtraverse_time_avg = 0.;
    //float parallel_bcktraverse_time_avg = 0.;
    float parallel_verify_time = 0;
    float parallel_reset_time = 0;
    size_t parallel_arr_req_verified = 0;
    std::map<std::string,float> parallel_prof_data;
    {
        cout << "Running Parrallel Analysis " << NUM_PARALLEL_RUNS << " times" << endl;

        for(int i = 0; i < NUM_PARALLEL_RUNS; i++) {
            //Analyze
            {
                auto start = Clock::now();

                parallel_analyzer->update_timing();

                parallel_prof_data["analysis_sec"] += std::chrono::duration_cast<dsec>(Clock::now() - start).count();
            }

            for(auto key : {"arrival_pre_traversal_sec", "arrival_traversal_sec", "required_pre_traversal_sec", "required_traversal_sec"}) {
                parallel_prof_data[key] += parallel_analyzer->get_profiling_data(key);
            }

            cout << ".";
            cout.flush();

            //Verify
            clock_gettime(CLOCK_MONOTONIC, &verify_start);

#ifdef TATUM_TATUM_ASSERT_VPR_TO_TATUM
            if(i == 0 || i == NUM_PARALLEL_RUNS - 1) {
                parallel_arr_req_verified = verify_analyzer(*timing_graph, parallel_analyzer,
                                                          expected_arr_req_times, const_gen_fanout_nodes,
                                                          clock_gen_fanout_nodes );
            }
#endif

            clock_gettime(CLOCK_MONOTONIC, &verify_end);
            parallel_verify_time += time_sec(verify_start, verify_end);

            if(i < NUM_PARALLEL_RUNS-1) {
                clock_gettime(CLOCK_MONOTONIC, &reset_start);
                parallel_analyzer->reset_timing();
                clock_gettime(CLOCK_MONOTONIC, &reset_end);
                parallel_reset_time += time_sec(reset_start, reset_end);
            }
        }
        for(auto kv : parallel_prof_data) {
            parallel_prof_data[kv.first] /= NUM_PARALLEL_RUNS;
        }
        cout << endl;

        cout << "Parallel Analysis took " << std::setprecision(6) << std::setw(6) << parallel_prof_data["analysis_sec"]*NUM_PARALLEL_RUNS << " sec, AVG: " << parallel_prof_data["analysis_sec"] << " s" << endl;

        cout << "\tArr Pre-traversal Avg: " << std::setprecision(6) << std::setw(6) << parallel_prof_data["arrival_pre_traversal_sec"] << " s";
        cout << " (" << std::setprecision(2) << parallel_prof_data["arrival_pre_traversal_sec"]/parallel_prof_data["analysis_sec"] << ")" << endl;

        cout << "\tReq Pre-traversal Avg: " << std::setprecision(6) << std::setw(6) << parallel_prof_data["required_pre_traversal_sec"] << " s";
        cout << " (" << std::setprecision(2) << parallel_prof_data["required_pre_traversal_sec"]/parallel_prof_data["analysis_sec"] << ")" << endl;

        cout << "\tArr     traversal Avg: " << std::setprecision(6) << std::setw(6) << parallel_prof_data["arrival_traversal_sec"] << " s";
        cout << " (" << std::setprecision(2) << parallel_prof_data["arrival_traversal_sec"]/parallel_prof_data["analysis_sec"] << ")" << endl;

        cout << "\tReq     traversal Avg: " << std::setprecision(6) << std::setw(6) << parallel_prof_data["required_traversal_sec"] << " s";
        cout << " (" << std::setprecision(2) << parallel_prof_data["required_traversal_sec"]/parallel_prof_data["analysis_sec"] << ")" << endl;

        cout << "Verifying Parallel Analysis took: " <<  parallel_verify_time<< " sec" << endl;
        if(parallel_arr_req_verified != 2 * timing_graph->nodes().size() * expected_arr_req_times.get_num_clocks()) { //2x for arr and req
            cout << "WARNING: Expected arr/req times differ from number of nodes. Verification may not have occured!" << endl;
        } else {
            cout << "\tVerified " << serial_arr_req_verified << " arr/req times accross " << timing_graph->nodes().size() << " nodes and " << expected_arr_req_times.get_num_clocks() << " clocks" << endl;
        }
        cout << "Resetting Parallel Analysis took: " << parallel_reset_time << " sec" << endl;
    }
    cout << endl;



    cout << "Parallel Speed-Up: " << std::fixed << serial_prof_data["analysis_sec"] / parallel_prof_data["analysis_sec"] << "x" << endl;
    cout << "\tArr Pre-traversal: " << std::fixed << serial_prof_data["arrival_pre_traversal_sec"] / parallel_prof_data["arrival_pre_traversal_sec"] << "x" << endl;
    cout << "\tReq Pre-traversal: " << std::fixed << serial_prof_data["required_pre_traversal_sec"] / parallel_prof_data["required_pre_traversal_sec"] << "x" << endl;
    cout << "\t    Arr-traversal: " << std::fixed << serial_prof_data["arrival_traversal_sec"] / parallel_prof_data["arrival_traversal_sec"] << "x" << endl;
    cout << "\t    Req-traversal: " << std::fixed << serial_prof_data["required_traversal_sec"] / parallel_prof_data["required_traversal_sec"] << "x" << endl;
    cout << endl;

    //Per-level speed-up
    dump_level_times("level_times.csv", *timing_graph, serial_prof_data, parallel_prof_data);
#endif //NUM_PARALLEL_RUNS


    //Tag stats
    print_setup_tags_histogram(*timing_graph, serial_analyzer);
    //print_hold_tags_histogram(timing_graph, serial_analyzer);

    clock_gettime(CLOCK_MONOTONIC, &prog_end);

    cout << endl << "Total time: " << time_sec(prog_start, prog_end) << " sec" << endl;

    return 0;
}
