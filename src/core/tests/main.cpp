/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
# include <iostream>
# include "gtest/gtest.h"
# include "test_utils.h"

# include <dsn/tool/simulator.h>
# include <dsn/tool/nativerun.h>
# include <dsn/tool/fastrun.h>
# include <dsn/toollet/tracer.h>
# include <dsn/toollet/profiler.h>
# include <dsn/toollet/fault_injector.h>

# include <dsn/tool/providers.common.h>
# include <dsn/tool/providers.hpc.h>
# include <dsn/tool/nfs_node_simple.h>

void module_init()
{
    // register all providers
    dsn::tools::register_common_providers();
    dsn::tools::register_hpc_providers();
    dsn::tools::register_component_provider<::dsn::service::nfs_node_simple>("dsn::service::nfs_node_simple");

    //dsn::tools::register_component_provider<dsn::thrift_binary_message_parser>("thrift");

    // register all possible tools and toollets
    dsn::tools::register_tool<dsn::tools::nativerun>("nativerun");
    dsn::tools::register_tool<dsn::tools::fastrun>("fastrun");
    dsn::tools::register_tool<dsn::tools::simulator>("simulator");
    dsn::tools::register_toollet<dsn::tools::tracer>("tracer");
    dsn::tools::register_toollet<dsn::tools::profiler>("profiler");
    dsn::tools::register_toollet<dsn::tools::fault_injector>("fault_injector");
}

GTEST_API_ int main(int argc, char **argv) 
{
    // register all tools
    module_init();

    // register all possible services
    dsn::register_app<test_client>("test");
    
    // specify what services and tools will run in config file, then run
    dsn_run(argc, argv, true);
    return 0;    
}