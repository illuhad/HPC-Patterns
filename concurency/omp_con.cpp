#define MAD_4(x, y)                                                            \
  x = y * x + y;                                                               \
  y = x * y + x;                                                               \
  x = y * x + y;                                                               \
  y = x * y + x;
#define MAD_16(x, y)                                                           \
  MAD_4(x, y);                                                                 \
  MAD_4(x, y);                                                                 \
  MAD_4(x, y);                                                                 \
  MAD_4(x, y);
#define MAD_64(x, y)                                                           \
  MAD_16(x, y);                                                                \
  MAD_16(x, y);                                                                \
  MAD_16(x, y);                                                                \
  MAD_16(x, y);

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#define NUM_REPETION 2

template <class T> T busy_wait(long N, T i) {
  T x = 1.3f;
  T y = (T)i;
  for (long j = 0; j < N; j++) {
    MAD_64(x, y);
  }
  return y;
}

template <class T>
void bench(std::vector<std::string> commands, long kernel_tripcount,
           bool enable_profiling, bool in_order, int n_queues, bool serial,
           long *total_cpu_time, long *max_cpu_time_command,
           int *max_index_cpu_time_command) {

  const int globalWIs =  16;
  const int N = 27000000;

  std::vector<T *> buffers;
  for (auto &command : commands) {
    std::vector<T *> buffer;
    char t = command[0];
    T *b;
    if (t == 'M') {
        b = static_cast<T *>(malloc(N * sizeof(T)));
        std::fill(b, b + N, 0);
        #pragma omp target enter data map(alloc: b[:N])
      } else if (t == 'D') {
        b = static_cast<T *>(malloc(N * sizeof(T)));
        #pragma omp target enter data map(alloc: b[:N])
      } else if (t == 'C') {
        b = static_cast<T *>(malloc(globalWIs * sizeof(T)));
        #pragma omp target enter data map(alloc: b[:globalWIs])
      }
   buffers.push_back(b);
  }

  *total_cpu_time = std::numeric_limits<long>::max();
  for (int r = 0; r < NUM_REPETION; r++) {
    std::vector<long> cpu_times;
    auto s0 = std::chrono::high_resolution_clock::now();

    // if parrallel paramm for and !serial
    for (int i = 0; i < commands.size(); i++) {
      const auto s = std::chrono::high_resolution_clock::now();
      if (commands[i] == "C") {
        T *ptr = buffers[i];
        #pragma omp target teams distribute parallel for nowait
        for (int j=0; j < globalWIs; j++)
          ptr[j] = busy_wait(kernel_tripcount, (T)j);
      } else if (commands[i] == "DM") {
        T *ptr=buffers[i];
        #pragma omp target update from(ptr[:N]) nowait 
      } else if (commands[i] == "MD") {
         T *ptr=buffers[i];
        #pragma omp target update to(ptr[:N]) nowait
      }
      if (serial) {
        #pragma omp taskwait
        const auto e = std::chrono::high_resolution_clock::now();
        cpu_times.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(e - s)
                .count());
      }
    }
    #pragma omp taskwait

    const auto e0 = std::chrono::high_resolution_clock::now();
    const auto curent_total_cpu_time =
        std::chrono::duration_cast<std::chrono::microseconds>(e0 - s0).count();
    if (curent_total_cpu_time < *total_cpu_time) {
      *total_cpu_time = curent_total_cpu_time;
      if (serial) {
        *max_index_cpu_time_command =
            std::distance(cpu_times.begin(),
                          std::max_element(cpu_times.begin(), cpu_times.end()));
        *max_cpu_time_command = cpu_times[*max_index_cpu_time_command];
      }
    }
  }


  for (const auto &ptr : buffers) {
    //#pragma omp target exit data 
    free(ptr);
  }
}

void print_help_and_exit(std::string binname, std::string msg) {
  if (!msg.empty())
    std::cout << "ERROR: " << msg << std::endl;
  std::string help = "Usage: " + binname +
                     " (in_order | out_of_order)\n"
                     "                [--enable_profiling]\n"
                     "                [--n_queues=<queues>]\n"
                     "                [--kernel_tripcount=<tripcount>]\n"
                     "                COMMAND...\n"
                     "\n"
                     "Options:\n"
                     "--kernel_tripcount       [default: 10000]\n"
                     "--n_queues=<nqueues>     [default: -1]. Number of queues "
                     "used to run COMMANDS. \n"
                     "                                        If -1: one queue "
                     "when out_of_order, one per COMMANDS when in order\n"
                     "COMMAND                  [possible values: C,MD,DM]\n"
                     "                            C:  Compute kernel \n"
                     "                            MD: Malloc allocated memory "
                     "to Device memory memcopy \n"
                     "                            DM: Device Memory to Malloc "
                     "allocated memory memcopy \n";
  std::cout << help << std::endl;
  std::exit(1);
}

int main(int argc, char *argv[]) {
  std::vector<std::string> argl(argv + 1, argv + argc);
  if (argl.empty())
    print_help_and_exit(argv[0], "");

  bool in_order = false;
  if ((argl[0] != "out_of_order") && (argl[0] != "in_order"))
    print_help_and_exit(argv[0], "Need to specify 'in_order' or 'out_of_order' option)");
  else if (argl[0] == "in_order")
    in_order = true;
  // in_order = false; mean out_of_order. I know stupid optimization

  bool enable_profiling = false;
  int n_queues = -1;
  std::vector<std::string> commands;
  long kernel_tripcount = 10000;

  // I'm just an old C programmer trying to do some C++
  for (int i = 1; i < argl.size(); i++) {
    std::string s{argl[i]};
    if (s == "--enable_profiling") {
      enable_profiling = true;
    } else if (s == "--queues") {
      i++;
      if (i < argl.size()) {
        n_queues = std::stoi(argl[i]);
      } else {
        print_help_and_exit(argv[0], "Need to specify an value for '--queues'");
      }
    } else if (s == "--kernel_tripcount") {
      i++;
      if (i < argl.size()) {
        kernel_tripcount = std::stol(argl[i]);
      } else {
        print_help_and_exit(argv[0],
                            "Need to specify an value for '--kernel_tripcount'");
      }
    } else if (s.rfind("-", 0) == 0) {
      print_help_and_exit(argv[0], "Unsuported option: '" + s + "'");
    } else {
      static std::vector<std::string> command_supported = {"C", "MD", "DM"};
      if (std::find(command_supported.begin(), command_supported.end(), s) ==
          command_supported.end())
        print_help_and_exit(argv[0], "Unsuported value for COMMAND");
      commands.push_back(s);
    }
  }
  if (n_queues == -1) {
    if (in_order)
      n_queues = 1;
    else
      n_queues = commands.size();
  }
  if (commands.empty())
    print_help_and_exit(argv[0], "Need to specify somme COMMAND and the order "
                                 "('--out_of_order' or '--in_order')");

  long serial_total_cpu_time;
  int serial_max_cpu_time_index_command;
  long serial_max_cpu_time_command;
  bench<float>(commands, kernel_tripcount, enable_profiling, in_order, n_queues,
               true, &serial_total_cpu_time, &serial_max_cpu_time_command,
               &serial_max_cpu_time_index_command);
  std::cout << "Total serial (us): " << serial_total_cpu_time
            << " (max commands (us) was "
            << commands[serial_max_cpu_time_index_command] << ": "
            << serial_max_cpu_time_command << ")" << std::endl;
  const double max_speedup =
      1. * serial_total_cpu_time / serial_max_cpu_time_command;
  std::cout << "Maximun Theoritical Speedup (assuming maximun concurency and "
               "negligeable runtime overhead) "
            << max_speedup << "x" << std::endl;
  if (max_speedup <= 1.30)
    std::cerr << "  WARNING: Large unblance between commands. Please play with '--kernel_tripcount' "
              << std::endl;

  long concurent_total_cpu_time;
  bench<float>(commands, kernel_tripcount, enable_profiling, in_order, n_queues,
               false, &concurent_total_cpu_time, NULL, NULL);
  std::cout << "Total // (us):     " << concurent_total_cpu_time << std::endl;
  std::cout << "Got " << 1. * serial_total_cpu_time / concurent_total_cpu_time
            << "x speed-up relative to serial" << std::endl;

  if (concurent_total_cpu_time <= 1.30 * serial_max_cpu_time_command) {
    std::cout << "SUCCESS: Concurent is faster than serial" << std::endl;
    return 0;
  }

  std::cout << "FAILURE: No Concurent Execution" << std::endl;
  return 1;
}
