#include <iostream>
#include <set>
#include <sstream>
#include <algorithm>

#include "Lower.h"

#include "AddImageChecks.h"
#include "AddParameterChecks.h"
#include "AllocationBoundsInference.h"
#include "Bounds.h"
#include "BoundsInference.h"
#include "CSE.h"
#include "CanonicalizeGPUVars.h"
#include "Debug.h"
#include "DebugArguments.h"
#include "DebugToFile.h"
#include "Deinterleave.h"
#include "EarlyFree.h"
#include "FindCalls.h"
#include "Func.h"
#include "Function.h"
#include "FuseGPUThreadLoops.h"
#include "FuzzFloatStores.h"
#include "HexagonOffload.h"
#include "InferArguments.h"
#include "InjectHostDevBufferCopies.h"
#include "InjectOpenGLIntrinsics.h"
#include "Inline.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "LICM.h"
#include "LoopCarry.h"
#include "Memoization.h"
#include "PartitionLoops.h"
#include "Prefetch.h"
#include "Profiling.h"
#include "Qualify.h"
#include "RealizationOrder.h"
#include "RemoveDeadAllocations.h"
#include "RemoveTrivialForLoops.h"
#include "RemoveUndef.h"
#include "ScheduleFunctions.h"
#include "SelectGPUAPI.h"
#include "SkipStages.h"
#include "SlidingWindow.h"
#include "Simplify.h"
#include "SimplifySpecializations.h"
#include "SplitTuples.h"
#include "StorageFlattening.h"
#include "StorageFolding.h"
#include "Substitute.h"
#include "Tracing.h"
#include "TrimNoOps.h"
#include "UnifyDuplicateLets.h"
#include "UniquifyVariableNames.h"
#include "UnpackBuffers.h"
#include "UnrollLoops.h"
#include "VaryingAttributes.h"
#include "VectorizeLoops.h"
#include "WrapCalls.h"
#include "WrapExternStages.h"

namespace Halide {
namespace Internal {

using std::set;
using std::ostringstream;
using std::string;
using std::vector;
using std::map;
using std::pair;

Module lower(const vector<Function> &output_funcs, const string &pipeline_name, const Target &t,
             const vector<Argument> &args, const Internal::LoweredFunc::LinkageType linkage_type,
             vector<string> &order, map<string, Function> &env,
             const vector<IRMutator *> &custom_passes,
             bool compile_to_tiramisu) {
    std::vector<std::string> namespaces;
    std::string simple_pipeline_name = extract_namespaces(pipeline_name, namespaces);

    Module result_module(simple_pipeline_name, t);

    // Compute an environment
    env.clear();
    for (Function f : output_funcs) {
        populate_environment(f, env);
    }

    // Create a deep-copy of the entire graph of Funcs.
    vector<Function> outputs;
    std::tie(outputs, env) = deep_copy(output_funcs, env);

    // Output functions should all be computed and stored at root.
    for (Function f: outputs) {
        Func(f).compute_root().store_root();
    }

    // Ensure that all ScheduleParams become well-defined constant Exprs.
    for (auto &f : env) {
        f.second.substitute_schedule_param_exprs();
    }

    map<pair<string, int>, vector<Dim>> old_dim_list;
    if (compile_to_tiramisu) {
        // We need to refresh the dim list since if split(), etc., are appplied
        // to the function, they mess up the dim list and create invalide
        // loops during schedule_functions.
        for (auto &iter : env) {
            Function func = iter.second;
            for (int i = 0; i < (int)func.updates().size() + 1; ++i) {
                Definition def = (i == 0) ? func.definition() : func.updates()[i-1];

                vector<Dim> old_dims = def.schedule().dims();
                old_dim_list.emplace(std::make_pair(func.name(), i), old_dims);

                // TODO(tiramisu): How to handle reordering with splits?
                vector<Dim> new_dims;
                set<string> seen_dims;
                for (int i = 0; i < (int)old_dims.size() - 1; ++i) {
                    Dim d = old_dims[i];
                    vector<string> v = split_string(d.var, ".");
                    internal_assert(!v.empty());
                    if (!seen_dims.count(v[0])) {
                        d.var = v[0];
                        new_dims.push_back(d);
                        seen_dims.insert(d.var);
                    }
                }

                // Add the __outermost dimension
                new_dims.push_back(old_dims[old_dims.size()-1]);
                def.schedule().dims() = new_dims;
            }
        }
    }

    // Substitute in wrapper Funcs
    env = wrap_func_calls(env);

    // Compute a realization order
    order.clear();
    order = realization_order(outputs, env);

    // Try to simplify the RHS/LHS of a function definition by propagating its
    // specializations' conditions
    simplify_specializations(env);

    debug(1) << "Creating initial loop nests...\n";
    bool any_memoized = false;
    Stmt s = schedule_functions(outputs, order, env, t, compile_to_tiramisu, any_memoized);
    debug(2) << "Lowering after creating initial loop nests:\n" << s << '\n';

    debug(1) << "Canonicalizing GPU var names...\n";
    s = canonicalize_gpu_vars(s);
    debug(2) << "Lowering after canonicalizing GPU var names:\n" << s << '\n';

    if (!compile_to_tiramisu) {
        if (any_memoized) {
            debug(1) << "Injecting memoization...\n";
            s = inject_memoization(s, env, pipeline_name, outputs);
            debug(2) << "Lowering after injecting memoization:\n" << s << '\n';
        } else {
            debug(1) << "Skipping injecting memoization...\n";
        }

        debug(1) << "Injecting tracing...\n";
        s = inject_tracing(s, pipeline_name, env, outputs, t);
        debug(2) << "Lowering after injecting tracing:\n" << s << '\n';

        debug(1) << "Adding checks for parameters\n";
        s = add_parameter_checks(s, t);
        debug(2) << "Lowering after injecting parameter checks:\n" << s << '\n';
    }

    // Compute the maximum and minimum possible value of each
    // function. Used in later bounds inference passes.
    debug(1) << "Computing bounds of each function's value\n";
    FuncValueBounds func_bounds = compute_function_value_bounds(order, env);

    if (!compile_to_tiramisu) {
        // The checks will be in terms of the symbols defined by bounds
        // inference.
        debug(1) << "Adding checks for images\n";
        s = add_image_checks(s, outputs, t, order, env, func_bounds);
        debug(2) << "Lowering after injecting image checks:\n" << s << '\n';
    }

    // This pass injects nested definitions of variable names, so we
    // can't simplify statements from here until we fix them up. (We
    // can still simplify Exprs).
    debug(1) << "Performing computation bounds inference...\n";
    s = bounds_inference(s, outputs, order, env, func_bounds, t);
    debug(2) << "Lowering after computation bounds inference:\n" << s << '\n';

    if (!compile_to_tiramisu) {
        debug(1) << "Performing sliding window optimization...\n";
        s = sliding_window(s, env);
        debug(2) << "Lowering after sliding window:\n" << s << '\n';
    }

    debug(1) << "Performing allocation bounds inference...\n";
    s = allocation_bounds_inference(s, env, func_bounds);
    debug(2) << "Lowering after allocation bounds inference:\n" << s << '\n';

    if (compile_to_tiramisu) {
        for (auto &iter : env) {
            Function func = iter.second;
            for (int i = 0; i < (int)func.updates().size() + 1; ++i) {
                Definition def = (i == 0) ? func.definition() : func.updates()[i-1];
                def.schedule().dims() = old_dim_list[std::make_pair(func.name(), i)];
            }
        }
    }

    debug(1) << "Removing code that depends on undef values...\n";
    s = remove_undef(s);
    debug(2) << "Lowering after removing code that depends on undef values:\n" << s << "\n\n";

    // This uniquifies the variable names, so we're good to simplify
    // after this point. This lets later passes assume syntactic
    // equivalence means semantic equivalence.
    debug(1) << "Uniquifying variable names...\n";
    s = uniquify_variable_names(s);
    debug(2) << "Lowering after uniquifying variable names:\n" << s << "\n\n";

    if (!compile_to_tiramisu) {
        // Storage flattening, etc, should be done by Tiramisu

        debug(1) << "Performing storage folding optimization...\n";
        s = storage_folding(s, env);
        debug(2) << "Lowering after storage folding:\n" << s << '\n';

        debug(1) << "Injecting debug_to_file calls...\n";
        s = debug_to_file(s, outputs, env);
        debug(2) << "Lowering after injecting debug_to_file calls:\n" << s << '\n';
    }

    debug(1) << "Simplifying...\n"; // without removing dead lets, because storage flattening needs the strides
    s = simplify(s, false);
    debug(2) << "Lowering after first simplification:\n" << s << "\n\n";

    if (!compile_to_tiramisu) {
        debug(1) << "Injecting prefetches...\n";
        s = inject_prefetch(s, env);
        debug(2) << "Lowering after injecting prefetches:\n" << s << "\n\n";

        debug(1) << "Dynamically skipping stages...\n";
        s = skip_stages(s, order);
        debug(2) << "Lowering after dynamically skipping stages:\n" << s << "\n\n";

        debug(1) << "Destructuring tuple-valued realizations...\n";
        s = split_tuples(s, env);
        debug(2) << "Lowering after destructuring tuple-valued realizations:\n" << s << "\n\n";

        debug(1) << "Performing storage flattening...\n";
        s = storage_flattening(s, outputs, env, t);
        debug(2) << "Lowering after storage flattening:\n" << s << "\n\n";

        debug(1) << "Unpacking buffer arguments...\n";
        s = unpack_buffers(s);
        debug(2) << "Lowering after unpacking buffer arguments...\n" << s << "\n\n";

        if (any_memoized) {
            debug(1) << "Rewriting memoized allocations...\n";
            s = rewrite_memoized_allocations(s, env);
            debug(2) << "Lowering after rewriting memoized allocations:\n" << s << "\n\n";
        } else {
            debug(1) << "Skipping rewriting memoized allocations...\n";
        }

        if (t.has_gpu_feature() ||
            t.has_feature(Target::OpenGLCompute) ||
            t.has_feature(Target::OpenGL) ||
            (t.arch != Target::Hexagon && (t.features_any_of({Target::HVX_64, Target::HVX_128})))) {
            debug(1) << "Selecting a GPU API for GPU loops...\n";
            s = select_gpu_api(s, t);
            debug(2) << "Lowering after selecting a GPU API:\n" << s << "\n\n";

            debug(1) << "Injecting host <-> dev buffer copies...\n";
            s = inject_host_dev_buffer_copies(s, t);
            debug(2) << "Lowering after injecting host <-> dev buffer copies:\n" << s << "\n\n";

            debug(1) << "Selecting a GPU API for extern stages...\n";
            s = select_gpu_api(s, t);
            debug(2) << "Lowering after selecting a GPU API for extern stages:\n" << s << "\n\n";
        }

        if (t.has_feature(Target::OpenGL)) {
            debug(1) << "Injecting OpenGL texture intrinsics...\n";
            s = inject_opengl_intrinsics(s);
            debug(2) << "Lowering after OpenGL intrinsics:\n" << s << "\n\n";
        }

        if (t.has_gpu_feature() ||
            t.has_feature(Target::OpenGLCompute)) {
            debug(1) << "Injecting per-block gpu synchronization...\n";
            s = fuse_gpu_thread_loops(s);
            debug(2) << "Lowering after injecting per-block gpu synchronization:\n" << s << "\n\n";
        }
    }

    if (!compile_to_tiramisu) {
        debug(1) << "Simplifying...\n";
        s = simplify(s);
        s = unify_duplicate_lets(s);
        s = remove_trivial_for_loops(s);
        debug(2) << "Lowering after second simplifcation:\n" << s << "\n\n";

        debug(1) << "Reduce prefetch dimension...\n";
        s = reduce_prefetch_dimension(s, t);
        debug(2) << "Lowering after reduce prefetch dimension:\n" << s << "\n";

        debug(1) << "Unrolling...\n";
        s = unroll_loops(s);
        s = simplify(s);
        debug(2) << "Lowering after unrolling:\n" << s << "\n\n";

        debug(1) << "Vectorizing...\n";
        s = vectorize_loops(s, t);
        s = simplify(s);
        debug(2) << "Lowering after vectorizing:\n" << s << "\n\n";

        debug(1) << "Detecting vector interleavings...\n";
        s = rewrite_interleavings(s);
        s = simplify(s);
        debug(2) << "Lowering after rewriting vector interleavings:\n" << s << "\n\n";

        debug(1) << "Partitioning loops to simplify boundary conditions...\n";
        s = partition_loops(s);
        s = simplify(s);
        debug(2) << "Lowering after partitioning loops:\n" << s << "\n\n";

        debug(1) << "Trimming loops to the region over which they do something...\n";
        s = trim_no_ops(s);
        debug(2) << "Lowering after loop trimming:\n" << s << "\n\n";

        debug(1) << "Injecting early frees...\n";
        s = inject_early_frees(s);
        debug(2) << "Lowering after injecting early frees:\n" << s << "\n\n";

        if (t.has_feature(Target::Profile)) {
            debug(1) << "Injecting profiling...\n";
            s = inject_profiling(s, pipeline_name);
            debug(2) << "Lowering after injecting profiling:\n" << s << "\n\n";
        }

        if (t.has_feature(Target::FuzzFloatStores)) {
            debug(1) << "Fuzzing floating point stores...\n";
            s = fuzz_float_stores(s);
            debug(2) << "Lowering after fuzzing floating point stores:\n" << s << "\n\n";
        }

        debug(1) << "Simplifying...\n";
        s = common_subexpression_elimination(s);
        s = loop_invariant_code_motion(s);

        if (t.has_feature(Target::OpenGL)) {
            debug(1) << "Detecting varying attributes...\n";
            s = find_linear_expressions(s);
            debug(2) << "Lowering after detecting varying attributes:\n" << s << "\n\n";

            debug(1) << "Moving varying attribute expressions out of the shader...\n";
            s = setup_gpu_vertex_buffer(s);
            debug(2) << "Lowering after removing varying attributes:\n" << s << "\n\n";
        }
    }

    // TODO(tiramisu): Tiramisu should have done this instead, but we'll use
    // Halide pass for now to remove the dead allocations.
    s = remove_dead_allocations(s);

    if (!compile_to_tiramisu) {
        s = remove_trivial_for_loops(s);
        s = simplify(s);
        debug(1) << "Lowering after final simplification:\n" << s << "\n\n";

        debug(1) << "Splitting off Hexagon offload...\n";
        s = inject_hexagon_rpc(s, t, result_module);
        debug(2) << "Lowering after splitting off Hexagon offload:\n" << s << '\n';

        if (!custom_passes.empty()) {
            for (size_t i = 0; i < custom_passes.size(); i++) {
                debug(1) << "Running custom lowering pass " << i << "...\n";
                s = custom_passes[i]->mutate(s);
                debug(1) << "Lowering after custom pass " << i << ":\n" << s << "\n\n";
            }
        }
    }

    vector<Argument> public_args = args;
    for (const auto &out : outputs) {
        for (Parameter buf : out.output_buffers()) {
            public_args.push_back(Argument(buf.name(),
                                           Argument::OutputBuffer,
                                           buf.type(), buf.dimensions()));
        }
    }

    vector<InferredArgument> inferred_args = infer_arguments(s, outputs);
    for (const InferredArgument &arg : inferred_args) {
        if (arg.param.defined() && arg.param.name() == "__user_context") {
            // The user context is always in the inferred args, but is
            // not required to be in the args list.
            continue;
        }

        internal_assert(arg.arg.is_input()) << "Expected only input Arguments here";

        bool found = false;
        for (Argument a : args) {
            found |= (a.name == arg.arg.name);
        }

        if (arg.buffer.defined() && !found) {
            // It's a raw Buffer used that isn't in the args
            // list. Embed it in the output instead.
            debug(1) << "Embedding image " << arg.buffer.name() << "\n";
            result_module.append(arg.buffer);
        } else if (!found) {
            std::ostringstream err;
            err << "Generated code refers to ";
            if (arg.arg.is_buffer()) {
                err << "image ";
            }
            err << "parameter " << arg.arg.name
                << ", which was not found in the argument list.\n";

            err << "\nArgument list specified: ";
            for (size_t i = 0; i < args.size(); i++) {
                err << args[i].name << " ";
            }
            err << "\n\nParameters referenced in generated code: ";
            for (const InferredArgument &ia : inferred_args) {
                if (ia.arg.name != "__user_context") {
                    err << ia.arg.name << " ";
                }
            }
            err << "\n\n";
            user_error << err.str();
        }
    }

    // We're about to drop the environment and outputs vector, which
    // contain the only strong refs to Functions that may still be
    // pointed to by the IR. So make those refs strong.
    class StrengthenRefs : public IRMutator {
        using IRMutator::visit;
        void visit(const Call *c) {
            IRMutator::visit(c);
            c = expr.as<Call>();
            internal_assert(c);
            if (c->func.defined()) {
                FunctionPtr ptr = c->func;
                ptr.strengthen();
                expr = Call::make(c->type, c->name, c->args, c->call_type,
                                  ptr, c->value_index,
                                  c->image, c->param);
            }
        }
    };
    s = StrengthenRefs().mutate(s);

    LoweredFunc main_func(pipeline_name, public_args, s, linkage_type);

    // If we're in debug mode, add code that prints the args.
    if (t.has_feature(Target::Debug)) {
        debug_arguments(&main_func);
    }

    result_module.append(main_func);

    // Append a wrapper for this pipeline that accepts old buffer_ts
    // and upgrades them. It will use the same name, so it will
    // require C++ linkage. We don't need it when jitting.
    if (!t.has_feature(Target::JIT)) {
        add_legacy_wrapper(result_module, main_func);
    }

    // Also append any wrappers for extern stages that expect the old buffer_t
    wrap_legacy_extern_stages(result_module);

    return result_module;
}

EXPORT Stmt lower_main_stmt(const std::vector<Function> &output_funcs, const std::string &pipeline_name,
                            const Target &t, vector<string> &order, map<string, Function> &env,
                            const std::vector<IRMutator *> &custom_passes,
                            bool compile_to_tiramisu) {
    // We really ought to start applying for appellation d'origine contr??l??e
    // status on types representing arguments in the Halide compiler.
    vector<InferredArgument> inferred_args = infer_arguments(Stmt(), output_funcs);
    vector<Argument> args;
    for (const auto &ia : inferred_args) {
        if (!ia.arg.name.empty() && ia.arg.is_input()) {
            args.push_back(ia.arg);
        }
    }

    Module module = lower(output_funcs, pipeline_name, t, args, Internal::LoweredFunc::External,
                          order, env, custom_passes, compile_to_tiramisu);

    return module.functions().front().body;
}

}
}
