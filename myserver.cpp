#include "server_http.hpp"
#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/process.hpp>
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>

#define TEST_BUCKET "code-test"

using namespace std;
using namespace boost::process;
using namespace boost::property_tree;
using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

typedef std::chrono::high_resolution_clock Clock;

string myexec(string cmd) {
    char buffer[128];
    string result = "";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw runtime_error("popen() failed!");
    try {
        while (!feof(pipe)) {
            if (fgets(buffer, 128, pipe) != NULL)
                result += buffer;
        }
    } catch (...) {
        pclose(pipe);
        throw;
    }
    pclose(pipe);
    return result;
}

//--------------------------------------------------------------------------------------------------
//------------------------------------------------- MAIN -------------------------------------------
//--------------------------------------------------------------------------------------------------
int main(int argc, char** argv)
{
    namespace po = boost::program_options;

    // Declare the supported options.
    po::options_description desc("Allowed options");
    desc.add_options()
            ("help", "produce help message")
            ("p", po::value<int>(), "set listening port")
            ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        cout << desc << "\n";
        return 1;
    }

    if (!vm.count("p")) {
        cout << "Port was not set.\n";
        return 1;
    }

    if (myexec("which sox") == "") {
       cerr << "Error: Sox not found" << endl;
       return 1;
    }

    // HTTP-server at port 8080 using 1 thread
    // Unless you do more heavy non-threaded processing in the resources,
    // 1 thread is usually faster than several threads
    HttpServer server;
    server.config.port = vm["p"].as<int>();
    server.config.thread_pool_size = 1;

    //*********** AWS **************
    Aws::SDKOptions options;
    options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
    Aws::InitAPI(options);

    // GET for the path /wav-info
    // Responds with JSON
    server.resource["^/wav-info$"]["GET"] = [](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
        //new tread
        thread work_thread([response, request] {
            auto t1 = Clock::now();
            string wavkey;
            stringstream out_stream;
            ptree json_arr;
            bool query_ok = false;
            auto query_fields = request->parse_query_string();

            for(auto &field : query_fields)
                if (field.first == "wavkey" && !field.second.empty())
                {
                    query_ok = true; //
                    wavkey =  field.second;
                    break;
                }

            if (query_ok) {
                //*********** AWS **************
                //set AWS region to us-east-2
                Aws::Client::ClientConfiguration clientConfig;
                clientConfig.region = "us-east-2";
                Aws::S3::S3Client s3_client(clientConfig);

                //AWS GET Object
                Aws::S3::Model::GetObjectRequest object_request3;
                object_request3.WithBucket(TEST_BUCKET).WithKey( wavkey.c_str() );
                auto get_object_outcome3 = s3_client.GetObject(object_request3);
                if (get_object_outcome3.IsSuccess())
                {
                    Aws::OFStream local_file;
                    //local temp file
                    boost::filesystem::path local_temp_file = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();

                    local_file.open(local_temp_file.c_str(), ios::out | ios::binary);
                    local_file << get_object_outcome3.GetResult().GetBody().rdbuf();

                    // ---------------------- SOX ---------------------
                    string cmd = str(boost::format("sox --i %1%") % local_temp_file);
                    string OUT = myexec(cmd.c_str());

                    // ---------------------- REGEX ---------------------
                    regex r1("Sample.Rate.+\\:.(\\w+)"); //
                    regex r2("Channels.+\\:.(\\w+)"); //
                    smatch m1,m2;
                    if (regex_search(OUT, m1, r1) && regex_search(OUT, m2, r2)) {
                        json_arr.put("sample_rate", m1[1]);
                        json_arr.put("channel_count", m2[1]);
                    } else {
                        json_arr.put("error", "SOX error, unsupported file");
                    }
                    //remove temp
                    boost::filesystem::remove(local_temp_file);
                } else {
                    json_arr.put("error", "AWS GetObject error");
                    cout << "GetObject error: " <<
                              get_object_outcome3.GetError().GetExceptionName() << " " <<
                              get_object_outcome3.GetError().GetMessage() << endl;
                }
            } else {
                json_arr.put("error", "empty wavkey");
            }

            auto t2 = Clock::now();
            json_arr.put("execution_time", chrono::duration_cast<chrono::microseconds>(t2 - t1).count()/10e5);
            write_json(out_stream, json_arr);
            response->write(out_stream);
        });
        work_thread.detach();
    };

    // GET for the path /mp3-to-wav
    // Responds with JSON
    server.resource["^/mp3-to-wav$"]["GET"] = [](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
        //new tread
        thread work_thread([response, request] {
            auto t1 = Clock::now();
            string mp3key, wavkey;
            stringstream out_stream;
            ptree json_arr;
            int query_ok = 0;
            auto query_fields = request->parse_query_string();

            for (auto &field : query_fields) {
                if (field.first == "wavkey" && !field.second.empty())
                {
                    query_ok++; //
                    wavkey = field.second;
                }
                if (field.first == "mp3key" && !field.second.empty())
                {
                    query_ok ++; //
                    mp3key =  field.second;
                }
            }

            if (query_ok == 2) {
                //*********** AWS **************
                //set AWS region to us-east-2
                Aws::Client::ClientConfiguration clientConfig;
                clientConfig.region = "us-east-2";
                Aws::S3::S3Client s3_client(clientConfig);

                //AWS GET Object
                Aws::S3::Model::GetObjectRequest object_request3;
                object_request3.WithBucket(TEST_BUCKET).WithKey( mp3key.c_str() );
                auto get_object_outcome3 = s3_client.GetObject(object_request3);
                if (get_object_outcome3.IsSuccess())
                {
                    Aws::OFStream local_file;
                    //local temp files
                    boost::filesystem::path local_temp_IN = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("%%%%-%%%%-%%%%-%%%%.mp3");
                    boost::filesystem::path local_temp_OUT = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("%%%%-%%%%-%%%%-%%%%.wav");

                    local_file.open(local_temp_IN.c_str(), ios::out | ios::binary);
                    local_file << get_object_outcome3.GetResult().GetBody().rdbuf();

                    // ---------------------- SOX ---------------------
                    string cmd = str(boost::format("sox %1% %2%") % local_temp_IN % local_temp_OUT);
                    string OUT = myexec(cmd.c_str());

                    if (OUT.empty()) {
                        //OK conversion
                        json_arr.put("file_size", boost::filesystem::file_size(local_temp_OUT));

                        //+++++++++++++++ Upload an Object
                        Aws::S3::Model::PutObjectRequest object_request;
                        object_request.WithBucket(TEST_BUCKET).WithKey(wavkey.c_str());
                        // Binary files must also have the std::ios_base::bin flag or'ed in
                        auto input_data = Aws::MakeShared<Aws::FStream>("PutObjectInputStream",
                                                                        local_temp_OUT.c_str(), ios_base::in | ios_base::binary);
                        object_request.SetBody(input_data);
                        auto put_object_outcome = s3_client.PutObject(object_request);
                        if (put_object_outcome.IsSuccess())
                        {
                            //std::cout << "Done upload!" << std::endl;
                        } else {
                            cout << "PutObject error: " <<
                                      put_object_outcome.GetError().GetExceptionName() << " " <<
                                      put_object_outcome.GetError().GetMessage() << endl;
                        }
                    }

                    //remove temp
                    boost::filesystem::remove(local_temp_OUT);
                    boost::filesystem::remove(local_temp_IN);

                } else {
                    json_arr.put("error", "AWS GetObject error");
                    cout << "GetObject error: " <<
                              get_object_outcome3.GetError().GetExceptionName() << " " <<
                              get_object_outcome3.GetError().GetMessage() << endl;
                }
            } else {
                json_arr.put("error", "empty wavkey or mp3key");
            }

            auto t2 = Clock::now();
            json_arr.put("execution_time", chrono::duration_cast<chrono::microseconds>(t2 - t1).count()/10e5);
            write_json(out_stream, json_arr);
            response->write(out_stream);
        });
        work_thread.detach();
    };

    // Default GET
    server.default_resource["GET"] = [](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
        //dummy
        auto query_fields = request->parse_query_string();
        response->write(SimpleWeb::StatusCode::client_error_bad_request, "Not implemented");
    };

    server.on_error = [](shared_ptr<HttpServer::Request> /*request*/, const SimpleWeb::error_code & /*ec*/) {
        // Handle errors here
        // Note that connection timeouts will also call this handle with ec set to SimpleWeb::errc::operation_canceled
    };

    thread server_thread([&server]() {
        // Start server
        server.start();
    });

    cout << "Server start listening at port " << vm["p"].as<int>() << endl;

    server_thread.join();
} //end main
