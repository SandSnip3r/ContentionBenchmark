# Synchronization Benchmark

## Problem

Two threads, `A` and `B`, need access to a shared resource. Thread `A` repeatedly needs the resource for a long time each time. Thread `B` needs the resource occasionally for a short time. Thread `B`'s latency is important. Because of thread `B`'s latency requirement, we will refer to it later as a **high priority** thread, while the other thread is considered **low priority**.

_Note that if thread `A` is in the middle of holding the resource, we will not interrupt `A`. The duration of `A`'s and `B`'s work can be assumed constant._

The two threads can be approximated with the following pseudocode:
```c++
// Thread A
while (1) {
  mutex.lock();
  // Do a lot of work with shared resource.
  mutex.unlock();
}
```
```c++
// Thread B
while (1) {
  // Do other work while not using shared resource.
  mutex.lock();
  // Do a little work with shared resource.
  mutex.unlock();
  // Do latency-sensitive work with result from shared resource.
}
```

## Real-life Scenario

This problem actually arose for me in a reinforcement learning project. Thread `A` is constantly training a neural network (the shared resource). Thread `B` occasionally needs to query the network to determine which action to take next. Since thread `B` is an actively controlled agent, low latency is important.

## Solutions

A naive solution would simply use a single mutex to protect the shared resource. However, mutexes give no guarantee of fairness. As a result, because of thread `A`'s  tight loop, it will often immediately relock the mutex after unlocking it. Because of the latency of OS scheduling, it is unlikely that thread `B` would ever get access to the mutex unless you implemented busy waiting with `mutex::try_lock()` in a tight loop. We would like a more elegant solution.

I stumbled upon [this answer from stack overflow](https://stackoverflow.com/a/11673600) which gives 3 solutions. The 3 solutions, for my specific problem, are as follows:

1. Use two mutexes, one guards the shared resource (`dataMutex`) as before and the other is used to "reserve" the next access to the shared resource (`nextAccessMutex`). Both threads have the following access pattern:
```c++
nextAccessMutex.lock();
dataMutex.lock();
nextAccessMutex.unlock();
// Use shared resource.
dataMutex.unlock();
```
2. Use a mutex (`dataMutex`), a condition variable (`cv`), and an atomic boolean (`waiting`). The boolean is used by the high priority thread to indicate that it is waiting for the resource. The low priority thread has the following access pattern:
```c++
std::unique_lock lock(dataMutex);
cv.wait(lock, [&]()->bool{
  return !waiting;
});
// Use shared resource.
lock.unlock();
cv.notify_one();
```
and the high priority thread has the following access pattern:
```c++
waiting = true;
std::unique_lock lock(dataMutex);
waiting = false;
// Use shared resource.
cv.notify_one();
lock.unlock();
```
3. Use a mutex (`dataMutex`), a condition variable (`cv`), and two non-atomic booleans (`waiting` and `dataHeld`). The condition variable and mutex protect the two booleans, not the data. The booleans protect the data. The low priority thread has the following access pattern:
```c++
std::unique_lock lock(dataMutex);
cv.wait(lock, [&]()->bool{
  return !(waiting || dataHeld);
});
dataHeld = true;
lock.unlock();
// Use shared resource.
lock.lock();
dataHeld = false;
lock.unlock();
cv.notify_one();
```
and the high priority thread has the following access pattern:
```c++
std::unique_lock lock(dataMutex);
waiting = true;
cv.wait(lock, [&]()->bool{
  return !dataHeld;
});
dataHeld = true;
lock.unlock();
// Use shared resource.
lock.lock();
dataHeld = false;
waiting = false;
lock.unlock();
cv.notify_one();
```

## Benchmark

These 3 solutions have varying complexity, which if peformance is equal, makes the two mutex solution the obvious choice. However, is performance equal? I created a benchmark `main.cpp` which tests these four different solutions (including the naive solution) over a range of different workload "types". I sweep over 3 workload parameters:

1. Duration of the low priority thread's hold on the shared resource.
2. Duration of the high priority thread's hold on the shared resource.
3. Duration of the high priority thread's "do other stuff" phase (not accessing the shared resource).

See below for the raw output of the benchmark. First I will give a summary. If the goal is to maximize the amount of work done by the low priority thread, the naive solution wins ~80% of the time. If the goal is to minimize the latency of the high priority thread, solution 2 (mutex, condition variable, and atomic-boolean) wins ~78% of the time. However, for my specific use case, the three above parameters are roughly as follows. The low priority thread spends a medium amount of time using the shared resource, the high priority thread spends a low amount of time using the shared resource, and the high priority thread spends a high amount of time doing work that does not require the shared resource. In this case, solution 2 minimizes latency for the high priority thread while also nearly maximizing time holding the shared resource in the low priority thread. _See below, parameters 1000,10,100000 and 1000,10,1000000 for a scenario like mine as I describe above._

## Data

### System Specs

OS: Debian-based Linux
Cpu: Intel Xeon W-2135

### Explanation of Data

As mentioned above, we sweep over 3 parameters. We use values in the range `{1, 10, 100, 1000, 10000, 100000, 1000000}` for each parameter (343 total tests). For each parameter choice, we test the 4 different algorithms. The reported `Low Priority` number is the number of nanoseconds that the low priority thread holds the shared resource. Maximizing this number is good. The reported `High Priority` number is the number of nanoseconds that the high priority thread is waiting for the shared resource. Minimizing this number is good. At the end of the data, results are reported which show how often each algorithm maximized `Low Priority` and minimized `High Priority`.

```
  Low Work,  High Work, High Sleep
         1,          1,          1
             BasicPriorityMutex Low Priority: 111454772392, High Priority: 119623575236
          TwoMutexPriorityMutex Low Priority:  47592138310, High Priority:  23973615088
MutexAndAtomicBoolPriorityMutex Low Priority:  49051247582, High Priority:  19478461968
   MutexAndTwoBoolPriorityMutex Low Priority:  48522464372, High Priority:  23756733222
         1,          1,         10
             BasicPriorityMutex Low Priority: 111266335257, High Priority: 119599119367
          TwoMutexPriorityMutex Low Priority:  49835123940, High Priority:  18117091304
MutexAndAtomicBoolPriorityMutex Low Priority:  53009462982, High Priority:   9014406131
   MutexAndTwoBoolPriorityMutex Low Priority:  48775203974, High Priority:  18834012280
         1,          1,        100
             BasicPriorityMutex Low Priority: 111233979429, High Priority: 119714996565
          TwoMutexPriorityMutex Low Priority:  77262326651, High Priority:  25767693917
MutexAndAtomicBoolPriorityMutex Low Priority:  78753353874, High Priority:  23128807572
   MutexAndTwoBoolPriorityMutex Low Priority:  78386077661, High Priority:  24652286942
         1,          1,       1000
             BasicPriorityMutex Low Priority: 111309255576, High Priority: 119193782587
          TwoMutexPriorityMutex Low Priority: 108658995229, High Priority:   5709954988
MutexAndAtomicBoolPriorityMutex Low Priority: 108974863149, High Priority:   5130081683
   MutexAndTwoBoolPriorityMutex Low Priority: 108822946228, High Priority:   5494828186
         1,          1,      10000
             BasicPriorityMutex Low Priority: 111511285456, High Priority: 115931817185
          TwoMutexPriorityMutex Low Priority: 118096854508, High Priority:    702116147
MutexAndAtomicBoolPriorityMutex Low Priority: 117961903938, High Priority:    616836783
   MutexAndTwoBoolPriorityMutex Low Priority: 117850626174, High Priority:    679547517
         1,          1,     100000
             BasicPriorityMutex Low Priority: 113414019436, High Priority:  89306751229
          TwoMutexPriorityMutex Low Priority: 119301210682, High Priority:     73891685
MutexAndAtomicBoolPriorityMutex Low Priority: 119119435413, High Priority:     61958134
   MutexAndTwoBoolPriorityMutex Low Priority: 119021603695, High Priority:     71144651
         1,          1,    1000000
             BasicPriorityMutex Low Priority: 117400131811, High Priority:  30135185025
          TwoMutexPriorityMutex Low Priority: 119427810773, High Priority:      7296834
MutexAndAtomicBoolPriorityMutex Low Priority: 119238810561, High Priority:      6295781
   MutexAndTwoBoolPriorityMutex Low Priority: 119148494550, High Priority:      7102898
         1,         10,          1
             BasicPriorityMutex Low Priority: 111185636566, High Priority: 119908125735
          TwoMutexPriorityMutex Low Priority:  44764781101, High Priority:  22696976243
MutexAndAtomicBoolPriorityMutex Low Priority:  46281019045, High Priority:  18281374008
   MutexAndTwoBoolPriorityMutex Low Priority:  45134581027, High Priority:  22496726168
         1,         10,         10
             BasicPriorityMutex Low Priority: 111258864114, High Priority: 119801409560
          TwoMutexPriorityMutex Low Priority:  46042459476, High Priority:  16401195839
MutexAndAtomicBoolPriorityMutex Low Priority:  49734580435, High Priority:   7598719181
   MutexAndTwoBoolPriorityMutex Low Priority:  45978003122, High Priority:  17435819213
         1,         10,        100
             BasicPriorityMutex Low Priority: 111242725753, High Priority: 119896939690
          TwoMutexPriorityMutex Low Priority:  75126213460, High Priority:  24970659748
MutexAndAtomicBoolPriorityMutex Low Priority:  76411203276, High Priority:  22532448931
   MutexAndTwoBoolPriorityMutex Low Priority:  76069931086, High Priority:  24084146451
         1,         10,       1000
             BasicPriorityMutex Low Priority: 111412304899, High Priority: 119485905798
          TwoMutexPriorityMutex Low Priority: 108291427123, High Priority:   5304682266
MutexAndAtomicBoolPriorityMutex Low Priority: 108239275930, High Priority:   4998098797
   MutexAndTwoBoolPriorityMutex Low Priority: 108047111883, High Priority:   5425284571
         1,         10,      10000
             BasicPriorityMutex Low Priority: 111501145444, High Priority: 115796167462
          TwoMutexPriorityMutex Low Priority: 117987953098, High Priority:    708504988
MutexAndAtomicBoolPriorityMutex Low Priority: 117864676668, High Priority:    624100596
   MutexAndTwoBoolPriorityMutex Low Priority: 117756805216, High Priority:    685577183
         1,         10,     100000
             BasicPriorityMutex Low Priority: 113290936844, High Priority:  89540689325
          TwoMutexPriorityMutex Low Priority: 119297436945, High Priority:     78260217
MutexAndAtomicBoolPriorityMutex Low Priority: 119108530654, High Priority:     64647708
   MutexAndTwoBoolPriorityMutex Low Priority: 119010572668, High Priority:     75362849
         1,         10,    1000000
             BasicPriorityMutex Low Priority: 117461291842, High Priority:  28983077502
          TwoMutexPriorityMutex Low Priority: 119417660969, High Priority:      7649785
MutexAndAtomicBoolPriorityMutex Low Priority: 119232854783, High Priority:      6351128
   MutexAndTwoBoolPriorityMutex Low Priority: 119146191276, High Priority:      7258694
         1,        100,          1
             BasicPriorityMutex Low Priority: 111229678934, High Priority: 119883355497
          TwoMutexPriorityMutex Low Priority:  29740293956, High Priority:  15339666758
MutexAndAtomicBoolPriorityMutex Low Priority:  30154501608, High Priority:  12164872748
   MutexAndTwoBoolPriorityMutex Low Priority:  29608401020, High Priority:  14994160111
         1,        100,         10
             BasicPriorityMutex Low Priority: 111221123468, High Priority: 119827394088
          TwoMutexPriorityMutex Low Priority:  30291228042, High Priority:  11349215836
MutexAndAtomicBoolPriorityMutex Low Priority:  31989960464, High Priority:   6073363966
   MutexAndTwoBoolPriorityMutex Low Priority:  30394452203, High Priority:  11970014745
         1,        100,        100
             BasicPriorityMutex Low Priority: 111215317112, High Priority: 119857009308
          TwoMutexPriorityMutex Low Priority:  57922900383, High Priority:  19349754112
MutexAndAtomicBoolPriorityMutex Low Priority:  58742181441, High Priority:  17475642384
   MutexAndTwoBoolPriorityMutex Low Priority:  58584688162, High Priority:  18562808983
         1,        100,       1000
             BasicPriorityMutex Low Priority: 111205911509, High Priority: 119500169725
          TwoMutexPriorityMutex Low Priority: 100404126443, High Priority:   5229180409
MutexAndAtomicBoolPriorityMutex Low Priority: 100716316012, High Priority:   4588718231
   MutexAndTwoBoolPriorityMutex Low Priority: 100499536576, High Priority:   5021721625
         1,        100,      10000
             BasicPriorityMutex Low Priority: 111544150736, High Priority: 116177640410
          TwoMutexPriorityMutex Low Priority: 116959017117, High Priority:    693283900
MutexAndAtomicBoolPriorityMutex Low Priority: 116827510417, High Priority:    612577723
   MutexAndTwoBoolPriorityMutex Low Priority: 116707792716, High Priority:    669262643
         1,        100,     100000
             BasicPriorityMutex Low Priority: 113277135996, High Priority:  89716368365
          TwoMutexPriorityMutex Low Priority: 119189539193, High Priority:     72499047
MutexAndAtomicBoolPriorityMutex Low Priority: 118999013539, High Priority:     62599973
   MutexAndTwoBoolPriorityMutex Low Priority: 118909881042, High Priority:     73431628
         1,        100,    1000000
             BasicPriorityMutex Low Priority: 117792322641, High Priority:  24028045232
          TwoMutexPriorityMutex Low Priority: 119418294923, High Priority:      7302489
MutexAndAtomicBoolPriorityMutex Low Priority: 119225848245, High Priority:      6136361
   MutexAndTwoBoolPriorityMutex Low Priority: 119134777284, High Priority:      7223452
         1,       1000,          1
             BasicPriorityMutex Low Priority: 110901426971, High Priority: 119450108563
          TwoMutexPriorityMutex Low Priority:   6884913096, High Priority:   6239448663
MutexAndAtomicBoolPriorityMutex Low Priority:   4606784819, High Priority:   3681984704
   MutexAndTwoBoolPriorityMutex Low Priority:   4648341047, High Priority:   4249829199
         1,       1000,         10
             BasicPriorityMutex Low Priority: 110779287156, High Priority: 119280610410
          TwoMutexPriorityMutex Low Priority:   7054519222, High Priority:   5642938422
MutexAndAtomicBoolPriorityMutex Low Priority:   6680022815, High Priority:   5659099677
   MutexAndTwoBoolPriorityMutex Low Priority:   6632233114, High Priority:   6415392406
         1,       1000,        100
             BasicPriorityMutex Low Priority: 110853260259, High Priority: 119440358994
          TwoMutexPriorityMutex Low Priority:  17060783843, High Priority:   5936462690
MutexAndAtomicBoolPriorityMutex Low Priority:  16495016849, High Priority:   5400439955
   MutexAndTwoBoolPriorityMutex Low Priority:  16478604637, High Priority:   5747506084
         1,       1000,       1000
             BasicPriorityMutex Low Priority: 110521865218, High Priority: 118938357927
          TwoMutexPriorityMutex Low Priority:  58875514558, High Priority:   2912056589
MutexAndAtomicBoolPriorityMutex Low Priority:  58973227629, High Priority:   2532673418
   MutexAndTwoBoolPriorityMutex Low Priority:  58849674247, High Priority:   2779902813
         1,       1000,      10000
             BasicPriorityMutex Low Priority: 111024874437, High Priority: 114468997060
          TwoMutexPriorityMutex Low Priority: 107174476279, High Priority:    639495010
MutexAndAtomicBoolPriorityMutex Low Priority: 107072270025, High Priority:    553489360
   MutexAndTwoBoolPriorityMutex Low Priority: 106967415768, High Priority:    614040071
         1,       1000,     100000
             BasicPriorityMutex Low Priority: 112804901931, High Priority:  90146619632
          TwoMutexPriorityMutex Low Priority: 118075185398, High Priority:     74163114
MutexAndAtomicBoolPriorityMutex Low Priority: 117887062546, High Priority:     64520044
   MutexAndTwoBoolPriorityMutex Low Priority: 117802146929, High Priority:     71226895
         1,       1000,    1000000
             BasicPriorityMutex Low Priority: 117260818746, High Priority:  30576051200
          TwoMutexPriorityMutex Low Priority: 119308195733, High Priority:      7352429
MutexAndAtomicBoolPriorityMutex Low Priority: 119113827437, High Priority:      6057229
   MutexAndTwoBoolPriorityMutex Low Priority: 119018600107, High Priority:      7036400
         1,      10000,          1
             BasicPriorityMutex Low Priority: 105287934790, High Priority: 113585416097
          TwoMutexPriorityMutex Low Priority:    842571759, High Priority:   1062965206
MutexAndAtomicBoolPriorityMutex Low Priority:    574561037, High Priority:    541443048
   MutexAndTwoBoolPriorityMutex Low Priority:    610475196, High Priority:    670732993
         1,      10000,         10
             BasicPriorityMutex Low Priority: 107091274556, High Priority: 115368271728
          TwoMutexPriorityMutex Low Priority:    854495557, High Priority:    944955828
MutexAndAtomicBoolPriorityMutex Low Priority:    805591170, High Priority:    793269873
   MutexAndTwoBoolPriorityMutex Low Priority:    802341439, High Priority:    897914766
         1,      10000,        100
             BasicPriorityMutex Low Priority: 107681564638, High Priority: 116066913421
          TwoMutexPriorityMutex Low Priority:   1702009069, High Priority:    762390965
MutexAndAtomicBoolPriorityMutex Low Priority:   1717901248, High Priority:    696550103
   MutexAndTwoBoolPriorityMutex Low Priority:   1714395207, High Priority:    768721398
         1,      10000,       1000
             BasicPriorityMutex Low Priority: 107709967809, High Priority: 115695470538
          TwoMutexPriorityMutex Low Priority:  11514507987, High Priority:    653210886
MutexAndAtomicBoolPriorityMutex Low Priority:  11538617925, High Priority:    584699259
   MutexAndTwoBoolPriorityMutex Low Priority:  11531484016, High Priority:    639074672
         1,      10000,      10000
             BasicPriorityMutex Low Priority: 108130408897, High Priority: 112940544007
          TwoMutexPriorityMutex Low Priority:  59362307790, High Priority:    366299981
MutexAndAtomicBoolPriorityMutex Low Priority:  59298152191, High Priority:    319096113
   MutexAndTwoBoolPriorityMutex Low Priority:  59236393129, High Priority:    350720067
         1,      10000,     100000
             BasicPriorityMutex Low Priority: 110584103596, High Priority:  89476366849
          TwoMutexPriorityMutex Low Priority: 108383630429, High Priority:     67290169
MutexAndAtomicBoolPriorityMutex Low Priority: 108222513237, High Priority:     58278189
   MutexAndTwoBoolPriorityMutex Low Priority: 108136987304, High Priority:     64029927
         1,      10000,    1000000
             BasicPriorityMutex Low Priority: 116371709752, High Priority:  32226331475
          TwoMutexPriorityMutex Low Priority: 118234379948, High Priority:      7388183
MutexAndAtomicBoolPriorityMutex Low Priority: 118056062404, High Priority:      6237195
   MutexAndTwoBoolPriorityMutex Low Priority: 117961725989, High Priority:      7226277
         1,     100000,          1
             BasicPriorityMutex Low Priority:  73708644072, High Priority:  79416990489
          TwoMutexPriorityMutex Low Priority:     87508926, High Priority:    112932255
MutexAndAtomicBoolPriorityMutex Low Priority:     47066677, High Priority:     43121913
   MutexAndTwoBoolPriorityMutex Low Priority:     48750175, High Priority:     53703005
         1,     100000,         10
             BasicPriorityMutex Low Priority:  79866429475, High Priority:  86130317559
          TwoMutexPriorityMutex Low Priority:     88244037, High Priority:    102401196
MutexAndAtomicBoolPriorityMutex Low Priority:     81440900, High Priority:     81724916
   MutexAndTwoBoolPriorityMutex Low Priority:     81223132, High Priority:     91188022
         1,     100000,        100
             BasicPriorityMutex Low Priority:  85449710454, High Priority:  92115698814
          TwoMutexPriorityMutex Low Priority:    166933232, High Priority:     82270344
MutexAndAtomicBoolPriorityMutex Low Priority:    174676045, High Priority:     74026672
   MutexAndTwoBoolPriorityMutex Low Priority:    170260767, High Priority:     81240312
         1,     100000,       1000
             BasicPriorityMutex Low Priority:  84208324465, High Priority:  90424875355
          TwoMutexPriorityMutex Low Priority:   1271838020, High Priority:     73430226
MutexAndAtomicBoolPriorityMutex Low Priority:   1281641780, High Priority:     66928832
   MutexAndTwoBoolPriorityMutex Low Priority:   1276615756, High Priority:     73823025
         1,     100000,      10000
             BasicPriorityMutex Low Priority:  85578671369, High Priority:  89227523470
          TwoMutexPriorityMutex Low Priority:  10916377666, High Priority:     66727590
MutexAndAtomicBoolPriorityMutex Low Priority:  10901250259, High Priority:     58371128
   MutexAndTwoBoolPriorityMutex Low Priority:  10896627543, High Priority:     64160860
         1,     100000,     100000
             BasicPriorityMutex Low Priority:  92108251783, High Priority:  75229964047
          TwoMutexPriorityMutex Low Priority:  59682499527, High Priority:     38075803
MutexAndAtomicBoolPriorityMutex Low Priority:  59585154224, High Priority:     33404524
   MutexAndTwoBoolPriorityMutex Low Priority:  59537986709, High Priority:     35484775
         1,     100000,    1000000
             BasicPriorityMutex Low Priority: 109125962832, High Priority:  24688618084
          TwoMutexPriorityMutex Low Priority: 108573478053, High Priority:      7015818
MutexAndAtomicBoolPriorityMutex Low Priority: 108395243669, High Priority:      5791429
   MutexAndTwoBoolPriorityMutex Low Priority: 108309675231, High Priority:      6477070
         1,    1000000,          1
             BasicPriorityMutex Low Priority:  16673863289, High Priority:  17978905579
          TwoMutexPriorityMutex Low Priority:      8744065, High Priority:     12223074
MutexAndAtomicBoolPriorityMutex Low Priority:      3752773, High Priority:      3494859
   MutexAndTwoBoolPriorityMutex Low Priority:      2790925, High Priority:      2898156
         1,    1000000,         10
             BasicPriorityMutex Low Priority:  21597490744, High Priority:  23273690023
          TwoMutexPriorityMutex Low Priority:      9024682, High Priority:     10584621
MutexAndAtomicBoolPriorityMutex Low Priority:      8070348, High Priority:      8022874
   MutexAndTwoBoolPriorityMutex Low Priority:      8383675, High Priority:      9528343
         1,    1000000,        100
             BasicPriorityMutex Low Priority:  25289186243, High Priority:  27229276054
          TwoMutexPriorityMutex Low Priority:     17191613, High Priority:      8157106
MutexAndAtomicBoolPriorityMutex Low Priority:     17039117, High Priority:      7381145
   MutexAndTwoBoolPriorityMutex Low Priority:     17278274, High Priority:      8120032
         1,    1000000,       1000
             BasicPriorityMutex Low Priority:  26881033957, High Priority:  28882982402
          TwoMutexPriorityMutex Low Priority:    129844968, High Priority:      7893566
MutexAndAtomicBoolPriorityMutex Low Priority:    130559318, High Priority:      7224981
   MutexAndTwoBoolPriorityMutex Low Priority:    129376215, High Priority:      7720208
         1,    1000000,      10000
             BasicPriorityMutex Low Priority:  25051681554, High Priority:  26035679826
          TwoMutexPriorityMutex Low Priority:   1197559755, High Priority:      7184964
MutexAndAtomicBoolPriorityMutex Low Priority:   1193381414, High Priority:      6777572
   MutexAndTwoBoolPriorityMutex Low Priority:   1191567391, High Priority:      6883569
         1,    1000000,     100000
             BasicPriorityMutex Low Priority:  30245382330, High Priority:  23073606712
          TwoMutexPriorityMutex Low Priority:  10922704942, High Priority:      6680426
MutexAndAtomicBoolPriorityMutex Low Priority:  10907296997, High Priority:      6001809
   MutexAndTwoBoolPriorityMutex Low Priority:  10899144371, High Priority:      5958141
         1,    1000000,    1000000
             BasicPriorityMutex Low Priority:  68017939614, High Priority:  18600998673
          TwoMutexPriorityMutex Low Priority:  59725469376, High Priority:      3732014
MutexAndAtomicBoolPriorityMutex Low Priority:  59631315809, High Priority:      3076970
   MutexAndTwoBoolPriorityMutex Low Priority:  59584736737, High Priority:      3352063
        10,          1,          1
             BasicPriorityMutex Low Priority: 112274848028, High Priority: 119929800481
          TwoMutexPriorityMutex Low Priority:  50926557082, High Priority:  28427853787
MutexAndAtomicBoolPriorityMutex Low Priority:  52286775045, High Priority:  24010374358
   MutexAndTwoBoolPriorityMutex Low Priority:  50951052402, High Priority:  28338110377
        10,          1,         10
             BasicPriorityMutex Low Priority: 112180377514, High Priority: 119918456839
          TwoMutexPriorityMutex Low Priority:  50926275068, High Priority:  22736798281
MutexAndAtomicBoolPriorityMutex Low Priority:  52604403709, High Priority:  18461598407
   MutexAndTwoBoolPriorityMutex Low Priority:  51548617776, High Priority:  22646663861
        10,          1,        100
             BasicPriorityMutex Low Priority: 112342789235, High Priority: 119824025512
          TwoMutexPriorityMutex Low Priority:  79056421068, High Priority:  28878412743
MutexAndAtomicBoolPriorityMutex Low Priority:  81172776229, High Priority:  27209577962
   MutexAndTwoBoolPriorityMutex Low Priority:  80599938607, High Priority:  29909593600
        10,          1,       1000
             BasicPriorityMutex Low Priority: 112260603246, High Priority: 119509577662
          TwoMutexPriorityMutex Low Priority: 108872044525, High Priority:   6438849011
MutexAndAtomicBoolPriorityMutex Low Priority: 109230400732, High Priority:   5790653232
   MutexAndTwoBoolPriorityMutex Low Priority: 109041259445, High Priority:   6235651856
        10,          1,      10000
             BasicPriorityMutex Low Priority: 112644007744, High Priority: 114723418225
          TwoMutexPriorityMutex Low Priority: 118166489957, High Priority:    756230654
MutexAndAtomicBoolPriorityMutex Low Priority: 118052612588, High Priority:    663788304
   MutexAndTwoBoolPriorityMutex Low Priority: 117948673462, High Priority:    722487703
        10,          1,     100000
             BasicPriorityMutex Low Priority: 113937253207, High Priority:  90041389046
          TwoMutexPriorityMutex Low Priority: 119362902765, High Priority:     77353607
MutexAndAtomicBoolPriorityMutex Low Priority: 119212288384, High Priority:     68418622
   MutexAndTwoBoolPriorityMutex Low Priority: 119125307719, High Priority:     75364005
        10,          1,    1000000
             BasicPriorityMutex Low Priority: 117954466583, High Priority:  25510185242
          TwoMutexPriorityMutex Low Priority: 119498800085, High Priority:      7551404
MutexAndAtomicBoolPriorityMutex Low Priority: 119327481324, High Priority:      6465057
   MutexAndTwoBoolPriorityMutex Low Priority: 119249089955, High Priority:      7250055
        10,         10,          1
             BasicPriorityMutex Low Priority: 112164148162, High Priority: 119921165724
          TwoMutexPriorityMutex Low Priority:  48325595995, High Priority:  27146396398
MutexAndAtomicBoolPriorityMutex Low Priority:  49633264093, High Priority:  22772615936
   MutexAndTwoBoolPriorityMutex Low Priority:  48628886013, High Priority:  26565283996
        10,         10,         10
             BasicPriorityMutex Low Priority: 112249610616, High Priority: 119929708500
          TwoMutexPriorityMutex Low Priority:  48429105731, High Priority:  21679713547
MutexAndAtomicBoolPriorityMutex Low Priority:  50082405284, High Priority:  17474237896
   MutexAndTwoBoolPriorityMutex Low Priority:  48689337988, High Priority:  21416309082
        10,         10,        100
             BasicPriorityMutex Low Priority: 112237727306, High Priority: 119851773779
          TwoMutexPriorityMutex Low Priority:  77073338081, High Priority:  27941694572
MutexAndAtomicBoolPriorityMutex Low Priority:  78923138365, High Priority:  26596333712
   MutexAndTwoBoolPriorityMutex Low Priority:  78553998779, High Priority:  29332620158
        10,         10,       1000
             BasicPriorityMutex Low Priority: 112222021011, High Priority: 119243774083
          TwoMutexPriorityMutex Low Priority: 108081376296, High Priority:   6376925994
MutexAndAtomicBoolPriorityMutex Low Priority: 108430815586, High Priority:   5748911552
   MutexAndTwoBoolPriorityMutex Low Priority: 108240291274, High Priority:   6215466577
        10,         10,      10000
             BasicPriorityMutex Low Priority: 112452955144, High Priority: 115422394814
          TwoMutexPriorityMutex Low Priority: 118060928361, High Priority:    772795438
MutexAndAtomicBoolPriorityMutex Low Priority: 117954385246, High Priority:    671156572
   MutexAndTwoBoolPriorityMutex Low Priority: 117840639015, High Priority:    730101851
        10,         10,     100000
             BasicPriorityMutex Low Priority: 113774232039, High Priority:  92142226343
          TwoMutexPriorityMutex Low Priority: 119362223887, High Priority:     74832016
MutexAndAtomicBoolPriorityMutex Low Priority: 119194173697, High Priority:     65631411
   MutexAndTwoBoolPriorityMutex Low Priority: 119114563793, High Priority:     72363199
        10,         10,    1000000
             BasicPriorityMutex Low Priority: 118106893176, High Priority:  23163793184
          TwoMutexPriorityMutex Low Priority: 119499718233, High Priority:      7652042
MutexAndAtomicBoolPriorityMutex Low Priority: 119328864134, High Priority:      6087789
   MutexAndTwoBoolPriorityMutex Low Priority: 119246890919, High Priority:      6946314
        10,        100,          1
             BasicPriorityMutex Low Priority: 112183917244, High Priority: 119881170670
          TwoMutexPriorityMutex Low Priority:  32376994259, High Priority:  18496094018
MutexAndAtomicBoolPriorityMutex Low Priority:  32957264026, High Priority:  15320408878
   MutexAndTwoBoolPriorityMutex Low Priority:  32419555108, High Priority:  18166464755
        10,        100,         10
             BasicPriorityMutex Low Priority: 112181616876, High Priority: 119866953005
          TwoMutexPriorityMutex Low Priority:  32636859517, High Priority:  14817867246
MutexAndAtomicBoolPriorityMutex Low Priority:  33343595942, High Priority:  11824808452
   MutexAndTwoBoolPriorityMutex Low Priority:  32640436291, High Priority:  14574006496
        10,        100,        100
             BasicPriorityMutex Low Priority: 112265817384, High Priority: 119828662608
          TwoMutexPriorityMutex Low Priority:  59224184640, High Priority:  21348828146
MutexAndAtomicBoolPriorityMutex Low Priority:  61187185977, High Priority:  20596138049
   MutexAndTwoBoolPriorityMutex Low Priority:  60904494022, High Priority:  22602414535
        10,        100,       1000
             BasicPriorityMutex Low Priority: 112346686475, High Priority: 119360535653
          TwoMutexPriorityMutex Low Priority: 100596686716, High Priority:   5895457720
MutexAndAtomicBoolPriorityMutex Low Priority: 100906951575, High Priority:   5319791722
   MutexAndTwoBoolPriorityMutex Low Priority: 100740900420, High Priority:   5702823824
        10,        100,      10000
             BasicPriorityMutex Low Priority: 112626475671, High Priority: 115221966090
          TwoMutexPriorityMutex Low Priority: 117015945145, High Priority:    772458255
MutexAndAtomicBoolPriorityMutex Low Priority: 116914666826, High Priority:    675009596
   MutexAndTwoBoolPriorityMutex Low Priority: 116810796331, High Priority:    734433673
        10,        100,     100000
             BasicPriorityMutex Low Priority: 114322822552, High Priority:  85828246050
          TwoMutexPriorityMutex Low Priority: 119256156114, High Priority:     75706999
MutexAndAtomicBoolPriorityMutex Low Priority: 119089519046, High Priority:     64348807
   MutexAndTwoBoolPriorityMutex Low Priority: 119008512467, High Priority:     73218983
        10,        100,    1000000
             BasicPriorityMutex Low Priority: 118064295072, High Priority:  23601600918
          TwoMutexPriorityMutex Low Priority: 119480888318, High Priority:      7468079
MutexAndAtomicBoolPriorityMutex Low Priority: 119318216961, High Priority:      6504104
   MutexAndTwoBoolPriorityMutex Low Priority: 119237694558, High Priority:      7039990
        10,       1000,          1
             BasicPriorityMutex Low Priority: 111773414329, High Priority: 119441114165
          TwoMutexPriorityMutex Low Priority:   7660368447, High Priority:   7538023944
MutexAndAtomicBoolPriorityMutex Low Priority:   5053496901, High Priority:   4260304846
   MutexAndTwoBoolPriorityMutex Low Priority:   5089461531, High Priority:   4800168049
        10,       1000,         10
             BasicPriorityMutex Low Priority: 111858975021, High Priority: 119491112838
          TwoMutexPriorityMutex Low Priority:   7711241062, High Priority:   6643804934
MutexAndAtomicBoolPriorityMutex Low Priority:   7318935379, High Priority:   6520776654
   MutexAndTwoBoolPriorityMutex Low Priority:   7286990572, High Priority:   7016517902
        10,       1000,        100
             BasicPriorityMutex Low Priority: 111720907998, High Priority: 119470932567
          TwoMutexPriorityMutex Low Priority:  16729966645, High Priority:   5847346694
MutexAndAtomicBoolPriorityMutex Low Priority:  16408137320, High Priority:   5843848241
   MutexAndTwoBoolPriorityMutex Low Priority:  16183748813, High Priority:   6169263704
        10,       1000,       1000
             BasicPriorityMutex Low Priority: 111759977739, High Priority: 118956854173
          TwoMutexPriorityMutex Low Priority:  59244067855, High Priority:   3404952823
MutexAndAtomicBoolPriorityMutex Low Priority:  59364967080, High Priority:   3012343115
   MutexAndTwoBoolPriorityMutex Low Priority:  59269189454, High Priority:   3267354053
        10,       1000,      10000
             BasicPriorityMutex Low Priority: 112186541040, High Priority: 114744443594
          TwoMutexPriorityMutex Low Priority: 107230761011, High Priority:    689408708
MutexAndAtomicBoolPriorityMutex Low Priority: 107154505435, High Priority:    605376328
   MutexAndTwoBoolPriorityMutex Low Priority: 107044597219, High Priority:    674998591
        10,       1000,     100000
             BasicPriorityMutex Low Priority: 114109387326, High Priority:  84676084618
          TwoMutexPriorityMutex Low Priority: 118218249380, High Priority:     70538416
MutexAndAtomicBoolPriorityMutex Low Priority: 118002922266, High Priority:     68103241
   MutexAndTwoBoolPriorityMutex Low Priority: 117893567369, High Priority:     77658432
        10,       1000,    1000000
             BasicPriorityMutex Low Priority: 118027590129, High Priority:  22883709820
          TwoMutexPriorityMutex Low Priority: 119377721248, High Priority:      7920730
MutexAndAtomicBoolPriorityMutex Low Priority: 119202319103, High Priority:      7061056
   MutexAndTwoBoolPriorityMutex Low Priority: 119123213140, High Priority:      7891201
        10,      10000,          1
             BasicPriorityMutex Low Priority: 106526656466, High Priority: 113803982186
          TwoMutexPriorityMutex Low Priority:    942308271, High Priority:   1161476502
MutexAndAtomicBoolPriorityMutex Low Priority:    598846649, High Priority:    595948666
   MutexAndTwoBoolPriorityMutex Low Priority:    621759826, High Priority:    676776115
        10,      10000,         10
             BasicPriorityMutex Low Priority: 107786865468, High Priority: 115188831935
          TwoMutexPriorityMutex Low Priority:    947331130, High Priority:   1070033529
MutexAndAtomicBoolPriorityMutex Low Priority:    900460032, High Priority:    881981914
   MutexAndTwoBoolPriorityMutex Low Priority:    891864942, High Priority:    956171331
        10,      10000,        100
             BasicPriorityMutex Low Priority: 107904651773, High Priority: 115278867187
          TwoMutexPriorityMutex Low Priority:   1744257544, High Priority:    814289419
MutexAndAtomicBoolPriorityMutex Low Priority:   1785328124, High Priority:    766661414
   MutexAndTwoBoolPriorityMutex Low Priority:   1775623864, High Priority:    824649248
        10,      10000,       1000
             BasicPriorityMutex Low Priority: 107876187325, High Priority: 114902979548
          TwoMutexPriorityMutex Low Priority:  11604884811, High Priority:    734025858
MutexAndAtomicBoolPriorityMutex Low Priority:  11598608680, High Priority:    645151480
   MutexAndTwoBoolPriorityMutex Low Priority:  11590359480, High Priority:    705464218
        10,      10000,      10000
             BasicPriorityMutex Low Priority: 108749506105, High Priority: 111763882933
          TwoMutexPriorityMutex Low Priority:  59411487911, High Priority:    379438864
MutexAndAtomicBoolPriorityMutex Low Priority:  59350252519, High Priority:    326170432
   MutexAndTwoBoolPriorityMutex Low Priority:  59301309595, High Priority:    361202663
        10,      10000,     100000
             BasicPriorityMutex Low Priority: 111182056428, High Priority:  85835269044
          TwoMutexPriorityMutex Low Priority: 108449673816, High Priority:     72213144
MutexAndAtomicBoolPriorityMutex Low Priority: 108299500646, High Priority:     62774479
   MutexAndTwoBoolPriorityMutex Low Priority: 108224072118, High Priority:     68469310
        10,      10000,    1000000
             BasicPriorityMutex Low Priority: 117055694373, High Priority:  25026931699
          TwoMutexPriorityMutex Low Priority: 118313899006, High Priority:      7696966
MutexAndAtomicBoolPriorityMutex Low Priority: 118146908707, High Priority:      6531945
   MutexAndTwoBoolPriorityMutex Low Priority: 118059199541, High Priority:      7342198
        10,     100000,          1
             BasicPriorityMutex Low Priority:  70852892816, High Priority:  75706681140
          TwoMutexPriorityMutex Low Priority:     97712041, High Priority:    125859080
MutexAndAtomicBoolPriorityMutex Low Priority:     50799885, High Priority:     53110900
   MutexAndTwoBoolPriorityMutex Low Priority:     53421621, High Priority:     57552135
        10,     100000,         10
             BasicPriorityMutex Low Priority:  80745367670, High Priority:  86184986670
          TwoMutexPriorityMutex Low Priority:     97458251, High Priority:    102409203
MutexAndAtomicBoolPriorityMutex Low Priority:     91335480, High Priority:     94256820
   MutexAndTwoBoolPriorityMutex Low Priority:     88523340, High Priority:    101096338
        10,     100000,        100
             BasicPriorityMutex Low Priority:  80337267624, High Priority:  85792587017
          TwoMutexPriorityMutex Low Priority:    172144257, High Priority:     80389726
MutexAndAtomicBoolPriorityMutex Low Priority:    177315384, High Priority:     69199345
   MutexAndTwoBoolPriorityMutex Low Priority:    172051552, High Priority:     81214900
        10,     100000,       1000
             BasicPriorityMutex Low Priority:  80412717638, High Priority:  85573769283
          TwoMutexPriorityMutex Low Priority:   1277725046, High Priority:     81983503
MutexAndAtomicBoolPriorityMutex Low Priority:   1285629148, High Priority:     74222961
   MutexAndTwoBoolPriorityMutex Low Priority:   1285038896, High Priority:     80942378
        10,     100000,      10000
             BasicPriorityMutex Low Priority:  84259268277, High Priority:  86825370191
          TwoMutexPriorityMutex Low Priority:  10927222051, High Priority:     72054843
MutexAndAtomicBoolPriorityMutex Low Priority:  10914050400, High Priority:     62880227
   MutexAndTwoBoolPriorityMutex Low Priority:  10909215128, High Priority:     68185292
        10,     100000,     100000
             BasicPriorityMutex Low Priority:  88968312888, High Priority:  66632819928
          TwoMutexPriorityMutex Low Priority:  59710808905, High Priority:     40024198
MutexAndAtomicBoolPriorityMutex Low Priority:  59633108683, High Priority:     34017977
   MutexAndTwoBoolPriorityMutex Low Priority:  59589879150, High Priority:     35894197
        10,     100000,    1000000
             BasicPriorityMutex Low Priority: 109384345972, High Priority:  22176242790
          TwoMutexPriorityMutex Low Priority: 108629587482, High Priority:      7288176
MutexAndAtomicBoolPriorityMutex Low Priority: 108476530944, High Priority:      6440268
   MutexAndTwoBoolPriorityMutex Low Priority: 108402020061, High Priority:      7038222
        10,    1000000,          1
             BasicPriorityMutex Low Priority:  15914965950, High Priority:  16990584070
          TwoMutexPriorityMutex Low Priority:      9750078, High Priority:     13006718
MutexAndAtomicBoolPriorityMutex Low Priority:      4540504, High Priority:      4690855
   MutexAndTwoBoolPriorityMutex Low Priority:      4007208, High Priority:      4178613
        10,    1000000,         10
             BasicPriorityMutex Low Priority:  22819300632, High Priority:  24376005368
          TwoMutexPriorityMutex Low Priority:     10106675, High Priority:     10884974
MutexAndAtomicBoolPriorityMutex Low Priority:      9232321, High Priority:      8215997
   MutexAndTwoBoolPriorityMutex Low Priority:      9552336, High Priority:     11366152
        10,    1000000,        100
             BasicPriorityMutex Low Priority:  20912722187, High Priority:  22331189549
          TwoMutexPriorityMutex Low Priority:     15977539, High Priority:      6357129
MutexAndAtomicBoolPriorityMutex Low Priority:     17875037, High Priority:      7913741
   MutexAndTwoBoolPriorityMutex Low Priority:     15921577, High Priority:      6818048
        10,    1000000,       1000
             BasicPriorityMutex Low Priority:  22590536064, High Priority:  24042369050
          TwoMutexPriorityMutex Low Priority:    130867253, High Priority:      8738754
MutexAndAtomicBoolPriorityMutex Low Priority:    130119883, High Priority:      7599253
   MutexAndTwoBoolPriorityMutex Low Priority:    130492278, High Priority:      8312721
        10,    1000000,      10000
             BasicPriorityMutex Low Priority:  21550624116, High Priority:  21993325824
          TwoMutexPriorityMutex Low Priority:   1194946686, High Priority:      7567517
MutexAndAtomicBoolPriorityMutex Low Priority:   1193326018, High Priority:      6298509
   MutexAndTwoBoolPriorityMutex Low Priority:   1193483811, High Priority:      7483255
        10,    1000000,     100000
             BasicPriorityMutex Low Priority:  29869169210, High Priority:  22415819523
          TwoMutexPriorityMutex Low Priority:  10929091836, High Priority:      7221105
MutexAndAtomicBoolPriorityMutex Low Priority:  10914234650, High Priority:      6415555
   MutexAndTwoBoolPriorityMutex Low Priority:  10905434474, High Priority:      6913628
        10,    1000000,    1000000
             BasicPriorityMutex Low Priority:  65930637920, High Priority:  12986575601
          TwoMutexPriorityMutex Low Priority:  59761049733, High Priority:      4109494
MutexAndAtomicBoolPriorityMutex Low Priority:  59675893647, High Priority:      3712347
   MutexAndTwoBoolPriorityMutex Low Priority:  59636467527, High Priority:      3822563
       100,          1,          1
             BasicPriorityMutex Low Priority: 116386027654, High Priority: 119968808187
          TwoMutexPriorityMutex Low Priority:  74000827825, High Priority:  59744784539
MutexAndAtomicBoolPriorityMutex Low Priority:  75420464157, High Priority:  57227508445
   MutexAndTwoBoolPriorityMutex Low Priority:  74234515153, High Priority:  59479921350
       100,          1,         10
             BasicPriorityMutex Low Priority: 116353229540, High Priority: 119974361343
          TwoMutexPriorityMutex Low Priority:  74046701266, High Priority:  55661451464
MutexAndAtomicBoolPriorityMutex Low Priority:  75426449538, High Priority:  53112563988
   MutexAndTwoBoolPriorityMutex Low Priority:  74236858354, High Priority:  55474695555
       100,          1,        100
             BasicPriorityMutex Low Priority: 116376531192, High Priority: 119956515345
          TwoMutexPriorityMutex Low Priority:  74460530948, High Priority:  15695261051
MutexAndAtomicBoolPriorityMutex Low Priority:  76073874582, High Priority:  12881283044
   MutexAndTwoBoolPriorityMutex Low Priority:  74908233318, High Priority:  15768446483
       100,          1,       1000
             BasicPriorityMutex Low Priority: 116382781548, High Priority: 119842178292
          TwoMutexPriorityMutex Low Priority: 109335786371, High Priority:   9947360077
MutexAndAtomicBoolPriorityMutex Low Priority: 109834710676, High Priority:   9660961116
   MutexAndTwoBoolPriorityMutex Low Priority: 109737131656, High Priority:  10051458674
       100,          1,      10000
             BasicPriorityMutex Low Priority: 116416179880, High Priority: 118504077833
          TwoMutexPriorityMutex Low Priority: 118413838441, High Priority:   1305065419
MutexAndAtomicBoolPriorityMutex Low Priority: 118397937402, High Priority:   1206343826
   MutexAndTwoBoolPriorityMutex Low Priority: 118340552950, High Priority:   1275426023
       100,          1,     100000
             BasicPriorityMutex Low Priority: 116735352712, High Priority: 106977389027
          TwoMutexPriorityMutex Low Priority: 119633221070, High Priority:    141592234
MutexAndAtomicBoolPriorityMutex Low Priority: 119561135031, High Priority:    127644747
   MutexAndTwoBoolPriorityMutex Low Priority: 119522224385, High Priority:    141160507
       100,          1,    1000000
             BasicPriorityMutex Low Priority: 118309495737, High Priority:  52124346004
          TwoMutexPriorityMutex Low Priority: 119762608926, High Priority:     13535684
MutexAndAtomicBoolPriorityMutex Low Priority: 119682691509, High Priority:     13229522
   MutexAndTwoBoolPriorityMutex Low Priority: 119646345176, High Priority:     14162324
       100,         10,          1
             BasicPriorityMutex Low Priority: 116381647274, High Priority: 119964662711
          TwoMutexPriorityMutex Low Priority:  71617918778, High Priority:  57688231168
MutexAndAtomicBoolPriorityMutex Low Priority:  72861763393, High Priority:  55244630036
   MutexAndTwoBoolPriorityMutex Low Priority:  71709625793, High Priority:  57497253419
       100,         10,         10
             BasicPriorityMutex Low Priority: 116364431259, High Priority: 119972887111
          TwoMutexPriorityMutex Low Priority:  71594773658, High Priority:  53764809927
MutexAndAtomicBoolPriorityMutex Low Priority:  72828550551, High Priority:  51341674798
   MutexAndTwoBoolPriorityMutex Low Priority:  71766699019, High Priority:  53615520810
       100,         10,        100
             BasicPriorityMutex Low Priority: 116378935747, High Priority: 119962579880
          TwoMutexPriorityMutex Low Priority:  72075843303, High Priority:  15196474161
MutexAndAtomicBoolPriorityMutex Low Priority:  73469091820, High Priority:  12344694828
   MutexAndTwoBoolPriorityMutex Low Priority:  72292210272, High Priority:  15065618835
       100,         10,       1000
             BasicPriorityMutex Low Priority: 116384563890, High Priority: 119804566040
          TwoMutexPriorityMutex Low Priority: 108623019980, High Priority:   9916014200
MutexAndAtomicBoolPriorityMutex Low Priority: 109073966641, High Priority:   9575205966
   MutexAndTwoBoolPriorityMutex Low Priority: 108950576585, High Priority:   9918735921
       100,         10,      10000
             BasicPriorityMutex Low Priority: 116414132087, High Priority: 118410429879
          TwoMutexPriorityMutex Low Priority: 118308080275, High Priority:   1309570066
MutexAndAtomicBoolPriorityMutex Low Priority: 118303233294, High Priority:   1202420449
   MutexAndTwoBoolPriorityMutex Low Priority: 118234212657, High Priority:   1266557974
       100,         10,     100000
             BasicPriorityMutex Low Priority: 116789719178, High Priority: 105572800099
          TwoMutexPriorityMutex Low Priority: 119612067685, High Priority:    140252752
MutexAndAtomicBoolPriorityMutex Low Priority: 119549670043, High Priority:    127890881
   MutexAndTwoBoolPriorityMutex Low Priority: 119509323702, High Priority:    135348990
       100,         10,    1000000
             BasicPriorityMutex Low Priority: 118144328213, High Priority:  58768492496
          TwoMutexPriorityMutex Low Priority: 119759716073, High Priority:     13188776
MutexAndAtomicBoolPriorityMutex Low Priority: 119681937683, High Priority:     12818483
   MutexAndTwoBoolPriorityMutex Low Priority: 119645256506, High Priority:     13473819
       100,        100,          1
             BasicPriorityMutex Low Priority: 116426689019, High Priority: 119951908768
          TwoMutexPriorityMutex Low Priority:  53763306872, High Priority:  43620154292
MutexAndAtomicBoolPriorityMutex Low Priority:  54431445238, High Priority:  41491929185
   MutexAndTwoBoolPriorityMutex Low Priority:  53813568903, High Priority:  43260064132
       100,        100,         10
             BasicPriorityMutex Low Priority: 116322703440, High Priority: 119946166963
          TwoMutexPriorityMutex Low Priority:  53794760699, High Priority:  40651052940
MutexAndAtomicBoolPriorityMutex Low Priority:  54499884458, High Priority:  38571463513
   MutexAndTwoBoolPriorityMutex Low Priority:  53924876331, High Priority:  40383708764
       100,        100,        100
             BasicPriorityMutex Low Priority: 116338810063, High Priority: 119915397246
          TwoMutexPriorityMutex Low Priority:  54259048041, High Priority:  11623337082
MutexAndAtomicBoolPriorityMutex Low Priority:  55058520780, High Priority:   9368379336
   MutexAndTwoBoolPriorityMutex Low Priority:  54375130839, High Priority:  11501982785
       100,        100,       1000
             BasicPriorityMutex Low Priority: 116368497043, High Priority: 119763789834
          TwoMutexPriorityMutex Low Priority: 101309703815, High Priority:   9117292731
MutexAndAtomicBoolPriorityMutex Low Priority: 101753590940, High Priority:   8831671461
   MutexAndTwoBoolPriorityMutex Low Priority: 101712552136, High Priority:   9181094449
       100,        100,      10000
             BasicPriorityMutex Low Priority: 116385277224, High Priority: 118807885126
          TwoMutexPriorityMutex Low Priority: 117275755480, High Priority:   1304342618
MutexAndAtomicBoolPriorityMutex Low Priority: 117262709830, High Priority:   1190791538
   MutexAndTwoBoolPriorityMutex Low Priority: 117213617814, High Priority:   1252676119
       100,        100,     100000
             BasicPriorityMutex Low Priority: 116701492379, High Priority: 108368125569
          TwoMutexPriorityMutex Low Priority: 119511010990, High Priority:    135376270
MutexAndAtomicBoolPriorityMutex Low Priority: 119441406148, High Priority:    126488965
   MutexAndTwoBoolPriorityMutex Low Priority: 119401610359, High Priority:    131480438
       100,        100,    1000000
             BasicPriorityMutex Low Priority: 118319652929, High Priority:  50979690551
          TwoMutexPriorityMutex Low Priority: 119750117080, High Priority:     16974886
MutexAndAtomicBoolPriorityMutex Low Priority: 119669247675, High Priority:     12543415
   MutexAndTwoBoolPriorityMutex Low Priority: 119635723260, High Priority:     12637150
       100,       1000,          1
             BasicPriorityMutex Low Priority: 116197549144, High Priority: 119717324102
          TwoMutexPriorityMutex Low Priority:  15267339155, High Priority:  15670276706
MutexAndAtomicBoolPriorityMutex Low Priority:  10298816099, High Priority:   9619228820
   MutexAndTwoBoolPriorityMutex Low Priority:  10568830927, High Priority:  10450673892
       100,       1000,         10
             BasicPriorityMutex Low Priority: 116221487524, High Priority: 119814658053
          TwoMutexPriorityMutex Low Priority:  15276144479, High Priority:  14819433889
MutexAndAtomicBoolPriorityMutex Low Priority:  14537734445, High Priority:  13633722772
   MutexAndTwoBoolPriorityMutex Low Priority:  14429147072, High Priority:  14351116974
       100,       1000,        100
             BasicPriorityMutex Low Priority: 116218120891, High Priority: 119783964898
          TwoMutexPriorityMutex Low Priority:  15376960368, High Priority:   6756008358
MutexAndAtomicBoolPriorityMutex Low Priority:  15471827023, High Priority:   6516684143
   MutexAndTwoBoolPriorityMutex Low Priority:  15375228327, High Priority:   7225229049
       100,       1000,       1000
             BasicPriorityMutex Low Priority: 116136371663, High Priority: 119467730012
          TwoMutexPriorityMutex Low Priority:  58159044695, High Priority:   5084327229
MutexAndAtomicBoolPriorityMutex Low Priority:  59952852837, High Priority:   4905570420
   MutexAndTwoBoolPriorityMutex Low Priority:  60003532341, High Priority:   5204653414
       100,       1000,      10000
             BasicPriorityMutex Low Priority: 116258541691, High Priority: 118355425623
          TwoMutexPriorityMutex Low Priority: 107167181826, High Priority:   1204168046
MutexAndAtomicBoolPriorityMutex Low Priority: 107402135258, High Priority:   1115492096
   MutexAndTwoBoolPriorityMutex Low Priority: 107332610684, High Priority:   1175021208
       100,       1000,     100000
             BasicPriorityMutex Low Priority: 116603941395, High Priority: 107147929230
          TwoMutexPriorityMutex Low Priority: 118366396241, High Priority:    129819057
MutexAndAtomicBoolPriorityMutex Low Priority: 118318539230, High Priority:    120320269
   MutexAndTwoBoolPriorityMutex Low Priority: 118281116870, High Priority:    129099600
       100,       1000,    1000000
             BasicPriorityMutex Low Priority: 118141923238, High Priority:  54923255486
          TwoMutexPriorityMutex Low Priority: 119631171518, High Priority:     13259641
MutexAndAtomicBoolPriorityMutex Low Priority: 119556584449, High Priority:     11586264
   MutexAndTwoBoolPriorityMutex Low Priority: 119524325149, High Priority:     13125776
       100,      10000,          1
             BasicPriorityMutex Low Priority: 113866559579, High Priority: 117299765495
          TwoMutexPriorityMutex Low Priority:   1985491955, High Priority:   2207839408
MutexAndAtomicBoolPriorityMutex Low Priority:   1361149433, High Priority:   1350709311
   MutexAndTwoBoolPriorityMutex Low Priority:   1356696002, High Priority:   1422862764
       100,      10000,         10
             BasicPriorityMutex Low Priority: 114873858265, High Priority: 118431925008
          TwoMutexPriorityMutex Low Priority:   1986090363, High Priority:   2122607666
MutexAndAtomicBoolPriorityMutex Low Priority:   1884582980, High Priority:   1864752063
   MutexAndTwoBoolPriorityMutex Low Priority:   1880469582, High Priority:   1969583253
       100,      10000,        100
             BasicPriorityMutex Low Priority: 114836438882, High Priority: 118354960291
          TwoMutexPriorityMutex Low Priority:   1996041522, High Priority:   1066434781
MutexAndAtomicBoolPriorityMutex Low Priority:   2007072589, High Priority:    961641190
   MutexAndTwoBoolPriorityMutex Low Priority:   1990062580, High Priority:   1035242562
       100,      10000,       1000
             BasicPriorityMutex Low Priority: 115107563069, High Priority: 118556730243
          TwoMutexPriorityMutex Low Priority:  11878181064, High Priority:   1115486179
MutexAndAtomicBoolPriorityMutex Low Priority:  11951518810, High Priority:   1011948255
   MutexAndTwoBoolPriorityMutex Low Priority:  11963760130, High Priority:   1106487713
       100,      10000,      10000
             BasicPriorityMutex Low Priority: 114740790688, High Priority: 116492819425
          TwoMutexPriorityMutex Low Priority:  59593382898, High Priority:    655659391
MutexAndAtomicBoolPriorityMutex Low Priority:  59645112310, High Priority:    625933282
   MutexAndTwoBoolPriorityMutex Low Priority:  59621427998, High Priority:    653311471
       100,      10000,     100000
             BasicPriorityMutex Low Priority: 115655257460, High Priority: 108325729322
          TwoMutexPriorityMutex Low Priority: 108670579150, High Priority:    119130531
MutexAndAtomicBoolPriorityMutex Low Priority: 108625633943, High Priority:    113095821
   MutexAndTwoBoolPriorityMutex Low Priority: 108585201360, High Priority:    120308602
       100,      10000,    1000000
             BasicPriorityMutex Low Priority: 117527276113, High Priority:  57151438283
          TwoMutexPriorityMutex Low Priority: 118571781628, High Priority:     13459305
MutexAndAtomicBoolPriorityMutex Low Priority: 118495003220, High Priority:     11650489
   MutexAndTwoBoolPriorityMutex Low Priority: 118457036207, High Priority:     12877027
       100,     100000,          1
             BasicPriorityMutex Low Priority: 102884662643, High Priority: 106072704713
          TwoMutexPriorityMutex Low Priority:    207301121, High Priority:    235651749
MutexAndAtomicBoolPriorityMutex Low Priority:    123496226, High Priority:    119534474
   MutexAndTwoBoolPriorityMutex Low Priority:    114894063, High Priority:    122165858
       100,     100000,         10
             BasicPriorityMutex Low Priority:  99811546091, High Priority: 102866070051
          TwoMutexPriorityMutex Low Priority:    206862098, High Priority:    222920257
MutexAndAtomicBoolPriorityMutex Low Priority:    195245336, High Priority:    193968620
   MutexAndTwoBoolPriorityMutex Low Priority:    192862372, High Priority:    206769889
       100,     100000,        100
             BasicPriorityMutex Low Priority: 104569438471, High Priority: 107779394268
          TwoMutexPriorityMutex Low Priority:    207930207, High Priority:    117754783
MutexAndAtomicBoolPriorityMutex Low Priority:    208975085, High Priority:    103994154
   MutexAndTwoBoolPriorityMutex Low Priority:    207891207, High Priority:    113281306
       100,     100000,       1000
             BasicPriorityMutex Low Priority: 104607840547, High Priority: 107747932287
          TwoMutexPriorityMutex Low Priority:   1323121291, High Priority:    127314057
MutexAndAtomicBoolPriorityMutex Low Priority:   1322152185, High Priority:    109969106
   MutexAndTwoBoolPriorityMutex Low Priority:   1331775840, High Priority:    128341656
       100,     100000,      10000
             BasicPriorityMutex Low Priority: 102433139027, High Priority: 104112424086
          TwoMutexPriorityMutex Low Priority:  11002843910, High Priority:    120091288
MutexAndAtomicBoolPriorityMutex Low Priority:  11004428433, High Priority:    115983251
   MutexAndTwoBoolPriorityMutex Low Priority:  11000602130, High Priority:    121069152
       100,     100000,     100000
             BasicPriorityMutex Low Priority: 105932360677, High Priority:  97673964793
          TwoMutexPriorityMutex Low Priority:  59867164699, High Priority:     67440676
MutexAndAtomicBoolPriorityMutex Low Priority:  59831537057, High Priority:     62941295
   MutexAndTwoBoolPriorityMutex Low Priority:  59816905662, High Priority:     68525351
       100,     100000,    1000000
             BasicPriorityMutex Low Priority: 112115478406, High Priority:  51673980143
          TwoMutexPriorityMutex Low Priority: 108870046652, High Priority:     12159721
MutexAndAtomicBoolPriorityMutex Low Priority: 108799890802, High Priority:     12101084
   MutexAndTwoBoolPriorityMutex Low Priority: 108768002287, High Priority:     11957603
       100,    1000000,          1
             BasicPriorityMutex Low Priority:  41310073978, High Priority:  42590463484
          TwoMutexPriorityMutex Low Priority:     20913173, High Priority:     23261380
MutexAndAtomicBoolPriorityMutex Low Priority:     11598383, High Priority:     10976095
   MutexAndTwoBoolPriorityMutex Low Priority:     11339890, High Priority:     11948512
       100,    1000000,         10
             BasicPriorityMutex Low Priority:  48020550091, High Priority:  49488814219
          TwoMutexPriorityMutex Low Priority:     20938856, High Priority:     22508670
MutexAndAtomicBoolPriorityMutex Low Priority:     19921908, High Priority:     20467021
   MutexAndTwoBoolPriorityMutex Low Priority:     19283934, High Priority:     20779297
       100,    1000000,        100
             BasicPriorityMutex Low Priority:  47504024563, High Priority:  48977599661
          TwoMutexPriorityMutex Low Priority:     21134220, High Priority:     13317247
MutexAndAtomicBoolPriorityMutex Low Priority:     21398546, High Priority:     11013262
   MutexAndTwoBoolPriorityMutex Low Priority:     20930579, High Priority:     11831393
       100,    1000000,       1000
             BasicPriorityMutex Low Priority:  53116152152, High Priority:  54690177605
          TwoMutexPriorityMutex Low Priority:    134692049, High Priority:     12079439
MutexAndAtomicBoolPriorityMutex Low Priority:    136385115, High Priority:     12140732
   MutexAndTwoBoolPriorityMutex Low Priority:    133727763, High Priority:     12042094
       100,    1000000,      10000
             BasicPriorityMutex Low Priority:  47510788347, High Priority:  48261552192
          TwoMutexPriorityMutex Low Priority:   1203288789, High Priority:     13204602
MutexAndAtomicBoolPriorityMutex Low Priority:   1202487198, High Priority:     11874248
   MutexAndTwoBoolPriorityMutex Low Priority:   1203034219, High Priority:     12746863
       100,    1000000,     100000
             BasicPriorityMutex Low Priority:  60152778616, High Priority:  55945709790
          TwoMutexPriorityMutex Low Priority:  10953633822, High Priority:     12513379
MutexAndAtomicBoolPriorityMutex Low Priority:  10944996011, High Priority:     12744927
   MutexAndTwoBoolPriorityMutex Low Priority:  10942883513, High Priority:     11330126
       100,    1000000,    1000000
             BasicPriorityMutex Low Priority:  75912401030, High Priority:  33059676202
          TwoMutexPriorityMutex Low Priority:  59896521605, High Priority:      7193955
MutexAndAtomicBoolPriorityMutex Low Priority:  59856092844, High Priority:      5836201
   MutexAndTwoBoolPriorityMutex Low Priority:  59838846237, High Priority:      5948773
      1000,          1,          1
             BasicPriorityMutex Low Priority: 119118214215, High Priority: 119998633163
          TwoMutexPriorityMutex Low Priority: 106453318364, High Priority: 106252006919
MutexAndAtomicBoolPriorityMutex Low Priority: 106797925445, High Priority: 105723478404
   MutexAndTwoBoolPriorityMutex Low Priority: 106373714081, High Priority: 106170041704
      1000,          1,         10
             BasicPriorityMutex Low Priority: 119130657501, High Priority: 119998326881
          TwoMutexPriorityMutex Low Priority: 106550715589, High Priority: 105360418012
MutexAndAtomicBoolPriorityMutex Low Priority: 107294817317, High Priority: 104932422264
   MutexAndTwoBoolPriorityMutex Low Priority: 106622113099, High Priority: 105344515649
      1000,          1,        100
             BasicPriorityMutex Low Priority: 119146558988, High Priority: 119992580592
          TwoMutexPriorityMutex Low Priority: 105318159786, High Priority:  96462329958
MutexAndAtomicBoolPriorityMutex Low Priority: 105545396115, High Priority:  96093446432
   MutexAndTwoBoolPriorityMutex Low Priority: 105130548316, High Priority:  96605047304
      1000,          1,       1000
             BasicPriorityMutex Low Priority: 119129526225, High Priority: 119981966532
          TwoMutexPriorityMutex Low Priority: 109908554494, High Priority:  19749137520
MutexAndAtomicBoolPriorityMutex Low Priority: 110168405918, High Priority:  18328567975
   MutexAndTwoBoolPriorityMutex Low Priority: 109724279592, High Priority:  18874985825
      1000,          1,      10000
             BasicPriorityMutex Low Priority: 119140612436, High Priority: 119745605170
          TwoMutexPriorityMutex Low Priority: 118319344826, High Priority:   5723122139
MutexAndAtomicBoolPriorityMutex Low Priority: 118323314762, High Priority:   4979308355
   MutexAndTwoBoolPriorityMutex Low Priority: 118301935966, High Priority:   5033090320
      1000,          1,     100000
             BasicPriorityMutex Low Priority: 119126519249, High Priority: 118898254604
          TwoMutexPriorityMutex Low Priority: 119689969457, High Priority:    762268662
MutexAndAtomicBoolPriorityMutex Low Priority: 119642709662, High Priority:    732507240
   MutexAndTwoBoolPriorityMutex Low Priority: 119630045911, High Priority:    721050889
      1000,          1,    1000000
             BasicPriorityMutex Low Priority: 119225129067, High Priority: 104981267081
          TwoMutexPriorityMutex Low Priority: 119848457060, High Priority:     72702564
MutexAndAtomicBoolPriorityMutex Low Priority: 119799307933, High Priority:     75568345
   MutexAndTwoBoolPriorityMutex Low Priority: 119786364437, High Priority:     82912314
      1000,         10,          1
             BasicPriorityMutex Low Priority: 119119683659, High Priority: 119998613233
          TwoMutexPriorityMutex Low Priority: 105786086845, High Priority: 105472858851
MutexAndAtomicBoolPriorityMutex Low Priority: 106313174198, High Priority: 105010139542
   MutexAndTwoBoolPriorityMutex Low Priority: 105631287739, High Priority: 105379600076
      1000,         10,         10
             BasicPriorityMutex Low Priority: 119134326674, High Priority: 119998416778
          TwoMutexPriorityMutex Low Priority: 105720613703, High Priority: 104565090774
MutexAndAtomicBoolPriorityMutex Low Priority: 106081683505, High Priority: 104137095415
   MutexAndTwoBoolPriorityMutex Low Priority: 105746877079, High Priority: 104561999847
      1000,         10,        100
             BasicPriorityMutex Low Priority: 119176752769, High Priority: 119996849111
          TwoMutexPriorityMutex Low Priority: 104444569576, High Priority:  95803810167
MutexAndAtomicBoolPriorityMutex Low Priority: 105019167619, High Priority:  95400757169
   MutexAndTwoBoolPriorityMutex Low Priority: 104598419233, High Priority:  95857691805
      1000,         10,       1000
             BasicPriorityMutex Low Priority: 119156749177, High Priority: 119950277021
          TwoMutexPriorityMutex Low Priority: 109145333946, High Priority:  19997376833
MutexAndAtomicBoolPriorityMutex Low Priority: 109447550311, High Priority:  18946605744
   MutexAndTwoBoolPriorityMutex Low Priority: 109060673659, High Priority:  19269917976
      1000,         10,      10000
             BasicPriorityMutex Low Priority: 119128594106, High Priority: 119858643830
          TwoMutexPriorityMutex Low Priority: 118219379171, High Priority:   5553977845
MutexAndAtomicBoolPriorityMutex Low Priority: 118231406983, High Priority:   4884248889
   MutexAndTwoBoolPriorityMutex Low Priority: 118203480417, High Priority:   5075842071
      1000,         10,     100000
             BasicPriorityMutex Low Priority: 119130345730, High Priority: 118998342295
          TwoMutexPriorityMutex Low Priority: 119680383335, High Priority:    745098883
MutexAndAtomicBoolPriorityMutex Low Priority: 119631991879, High Priority:    732033331
   MutexAndTwoBoolPriorityMutex Low Priority: 119620824760, High Priority:    751395593
      1000,         10,    1000000
             BasicPriorityMutex Low Priority: 119311055073, High Priority: 108998215610
          TwoMutexPriorityMutex Low Priority: 119859076510, High Priority:     77421465
MutexAndAtomicBoolPriorityMutex Low Priority: 119801454343, High Priority:     75122920
   MutexAndTwoBoolPriorityMutex Low Priority: 119787280387, High Priority:     68693972
      1000,        100,          1
             BasicPriorityMutex Low Priority: 119126438472, High Priority: 119996388276
          TwoMutexPriorityMutex Low Priority:  97279972096, High Priority:  98313231921
MutexAndAtomicBoolPriorityMutex Low Priority:  97570382842, High Priority:  97703791232
   MutexAndTwoBoolPriorityMutex Low Priority:  97345788689, High Priority:  98155496136
      1000,        100,         10
             BasicPriorityMutex Low Priority: 119116874945, High Priority: 119996849129
          TwoMutexPriorityMutex Low Priority:  97383318162, High Priority:  97470985349
MutexAndAtomicBoolPriorityMutex Low Priority:  97750072332, High Priority:  96958146384
   MutexAndTwoBoolPriorityMutex Low Priority:  97308010247, High Priority:  97401627555
      1000,        100,        100
             BasicPriorityMutex Low Priority: 119122601576, High Priority: 119992595417
          TwoMutexPriorityMutex Low Priority:  98426424638, High Priority:  89203477001
MutexAndAtomicBoolPriorityMutex Low Priority:  99032421384, High Priority:  88767484904
   MutexAndTwoBoolPriorityMutex Low Priority:  98380618851, High Priority:  89316232215
      1000,        100,       1000
             BasicPriorityMutex Low Priority: 119144724022, High Priority: 119975094618
          TwoMutexPriorityMutex Low Priority: 102798792121, High Priority:  21313692326
MutexAndAtomicBoolPriorityMutex Low Priority: 102847124484, High Priority:  19095803645
   MutexAndTwoBoolPriorityMutex Low Priority: 102514497775, High Priority:  19557092648
      1000,        100,      10000
             BasicPriorityMutex Low Priority: 119139146448, High Priority: 119897568457
          TwoMutexPriorityMutex Low Priority: 117218411333, High Priority:   5703526956
MutexAndAtomicBoolPriorityMutex Low Priority: 117230091562, High Priority:   4838676164
   MutexAndTwoBoolPriorityMutex Low Priority: 117198818905, High Priority:   4794989273
      1000,        100,     100000
             BasicPriorityMutex Low Priority: 119140569453, High Priority: 118797145850
          TwoMutexPriorityMutex Low Priority: 119575430424, High Priority:    709335148
MutexAndAtomicBoolPriorityMutex Low Priority: 119528884832, High Priority:    722735233
   MutexAndTwoBoolPriorityMutex Low Priority: 119512646839, High Priority:    744953268
      1000,        100,    1000000
             BasicPriorityMutex Low Priority: 119189418258, High Priority: 108998222194
          TwoMutexPriorityMutex Low Priority: 119838224519, High Priority:     72872589
MutexAndAtomicBoolPriorityMutex Low Priority: 119787724380, High Priority:     67549800
   MutexAndTwoBoolPriorityMutex Low Priority: 119773932228, High Priority:     75048386
      1000,       1000,          1
             BasicPriorityMutex Low Priority: 119086685320, High Priority: 119952392679
          TwoMutexPriorityMutex Low Priority:  56377978403, High Priority:  59260419179
MutexAndAtomicBoolPriorityMutex Low Priority:  45410916228, High Priority:  46936429281
   MutexAndTwoBoolPriorityMutex Low Priority:  43979959847, High Priority:  45919963238
      1000,       1000,         10
             BasicPriorityMutex Low Priority: 119079234638, High Priority: 119943652243
          TwoMutexPriorityMutex Low Priority:  56373521285, High Priority:  58650299849
MutexAndAtomicBoolPriorityMutex Low Priority:  55280286245, High Priority:  57269885285
   MutexAndTwoBoolPriorityMutex Low Priority:  55110175799, High Priority:  57569236312
      1000,       1000,        100
             BasicPriorityMutex Low Priority: 119126553461, High Priority: 119990174042
          TwoMutexPriorityMutex Low Priority:  56249927626, High Priority:  54030445042
MutexAndAtomicBoolPriorityMutex Low Priority:  56754853248, High Priority:  54070427751
   MutexAndTwoBoolPriorityMutex Low Priority:  56555451808, High Priority:  54383576223
      1000,       1000,       1000
             BasicPriorityMutex Low Priority: 119160581859, High Priority: 119945563277
          TwoMutexPriorityMutex Low Priority:  62857287117, High Priority:  14625392643
MutexAndAtomicBoolPriorityMutex Low Priority:  62193343447, High Priority:  12430098250
   MutexAndTwoBoolPriorityMutex Low Priority:  62092177981, High Priority:  13217720866
      1000,       1000,      10000
             BasicPriorityMutex Low Priority: 119107487768, High Priority: 119877605372
          TwoMutexPriorityMutex Low Priority: 107245626412, High Priority:   4532685670
MutexAndAtomicBoolPriorityMutex Low Priority: 107260108931, High Priority:   4131640805
   MutexAndTwoBoolPriorityMutex Low Priority: 107201430798, High Priority:   4054319201
      1000,       1000,     100000
             BasicPriorityMutex Low Priority: 119125326739, High Priority: 118382621461
          TwoMutexPriorityMutex Low Priority: 118400181499, High Priority:    708657065
MutexAndAtomicBoolPriorityMutex Low Priority: 118363600072, High Priority:    716370044
   MutexAndTwoBoolPriorityMutex Low Priority: 118346185226, High Priority:    728100838
      1000,       1000,    1000000
             BasicPriorityMutex Low Priority: 119223440456, High Priority: 101979772795
          TwoMutexPriorityMutex Low Priority: 119717940857, High Priority:     75357469
MutexAndAtomicBoolPriorityMutex Low Priority: 119671068473, High Priority:     65219546
   MutexAndTwoBoolPriorityMutex Low Priority: 119655623883, High Priority:     78849537
      1000,      10000,          1
             BasicPriorityMutex Low Priority: 118754845564, High Priority: 119611789935
          TwoMutexPriorityMutex Low Priority:  11747031267, High Priority:  12443284801
MutexAndAtomicBoolPriorityMutex Low Priority:   7997765952, High Priority:   8292178785
   MutexAndTwoBoolPriorityMutex Low Priority:   8463589574, High Priority:   8858541360
      1000,      10000,         10
             BasicPriorityMutex Low Priority: 118994832684, High Priority: 119867797597
          TwoMutexPriorityMutex Low Priority:  11755093793, High Priority:  12382767932
MutexAndAtomicBoolPriorityMutex Low Priority:  11174966067, High Priority:  11506060847
   MutexAndTwoBoolPriorityMutex Low Priority:  11319211245, High Priority:  11831258666
      1000,      10000,        100
             BasicPriorityMutex Low Priority: 118966008741, High Priority: 119846286236
          TwoMutexPriorityMutex Low Priority:  11746691572, High Priority:  11386831296
MutexAndAtomicBoolPriorityMutex Low Priority:  11820940761, High Priority:  11321687599
   MutexAndTwoBoolPriorityMutex Low Priority:  11821531551, High Priority:  11469900594
      1000,      10000,       1000
             BasicPriorityMutex Low Priority: 118973034330, High Priority: 119831921677
          TwoMutexPriorityMutex Low Priority:  12986607439, High Priority:   2280764966
MutexAndAtomicBoolPriorityMutex Low Priority:  12768690723, High Priority:   1906288784
   MutexAndTwoBoolPriorityMutex Low Priority:  12914447165, High Priority:   2169289502
      1000,      10000,      10000
             BasicPriorityMutex Low Priority: 118983928854, High Priority: 119665176791
          TwoMutexPriorityMutex Low Priority:  60454657222, High Priority:   2340808099
MutexAndAtomicBoolPriorityMutex Low Priority:  60421276928, High Priority:   2212991399
   MutexAndTwoBoolPriorityMutex Low Priority:  60382936190, High Priority:   2223576390
      1000,      10000,     100000
             BasicPriorityMutex Low Priority: 118970158565, High Priority: 118027256052
          TwoMutexPriorityMutex Low Priority: 108777949534, High Priority:    655698525
MutexAndAtomicBoolPriorityMutex Low Priority: 108742686817, High Priority:    614730016
   MutexAndTwoBoolPriorityMutex Low Priority: 108722109134, High Priority:    649171138
      1000,      10000,    1000000
             BasicPriorityMutex Low Priority: 119091441779, High Priority: 107887766832
          TwoMutexPriorityMutex Low Priority: 118658360694, High Priority:     71255930
MutexAndAtomicBoolPriorityMutex Low Priority: 118610219498, High Priority:     74760602
   MutexAndTwoBoolPriorityMutex Low Priority: 118598493891, High Priority:     73998490
      1000,     100000,          1
             BasicPriorityMutex Low Priority: 116839158193, High Priority: 117696569898
          TwoMutexPriorityMutex Low Priority:   1326698406, High Priority:   1402740975
MutexAndAtomicBoolPriorityMutex Low Priority:    784945562, High Priority:    806960138
   MutexAndTwoBoolPriorityMutex Low Priority:    697630010, High Priority:    732167379
      1000,     100000,         10
             BasicPriorityMutex Low Priority: 117744504725, High Priority: 118598549770
          TwoMutexPriorityMutex Low Priority:   1326250977, High Priority:   1393192563
MutexAndAtomicBoolPriorityMutex Low Priority:   1186478629, High Priority:   1233695135
   MutexAndTwoBoolPriorityMutex Low Priority:   1276119525, High Priority:   1328394142
      1000,     100000,        100
             BasicPriorityMutex Low Priority: 118030902352, High Priority: 118897305105
          TwoMutexPriorityMutex Low Priority:   1326982437, High Priority:   1289428781
MutexAndAtomicBoolPriorityMutex Low Priority:   1330305512, High Priority:   1280444824
   MutexAndTwoBoolPriorityMutex Low Priority:   1330730296, High Priority:   1292049759
      1000,     100000,       1000
             BasicPriorityMutex Low Priority: 117837019747, High Priority: 118683299145
          TwoMutexPriorityMutex Low Priority:   1421869886, High Priority:    211900990
MutexAndAtomicBoolPriorityMutex Low Priority:   1428519395, High Priority:    194100255
   MutexAndTwoBoolPriorityMutex Low Priority:   1443118785, High Priority:    236152236
      1000,     100000,      10000
             BasicPriorityMutex Low Priority: 117481767448, High Priority: 118117095568
          TwoMutexPriorityMutex Low Priority:  11310633960, High Priority:    462554293
MutexAndAtomicBoolPriorityMutex Low Priority:  11321855135, High Priority:    464526792
   MutexAndTwoBoolPriorityMutex Low Priority:  11287263477, High Priority:    425249843
      1000,     100000,     100000
             BasicPriorityMutex Low Priority: 118133383464, High Priority: 117898396647
          TwoMutexPriorityMutex Low Priority:  60072530815, High Priority:    375432832
MutexAndAtomicBoolPriorityMutex Low Priority:  60058744113, High Priority:    352099194
   MutexAndTwoBoolPriorityMutex Low Priority:  60045146781, High Priority:    368467978
      1000,     100000,    1000000
             BasicPriorityMutex Low Priority: 117994176285, High Priority: 105797647258
          TwoMutexPriorityMutex Low Priority: 108948272286, High Priority:     64863248
MutexAndAtomicBoolPriorityMutex Low Priority: 108908523737, High Priority:     66789314
   MutexAndTwoBoolPriorityMutex Low Priority: 108895020471, High Priority:     66189493
      1000,    1000000,          1
             BasicPriorityMutex Low Priority: 106206608568, High Priority: 106997903926
          TwoMutexPriorityMutex Low Priority:    135616952, High Priority:    143614808
MutexAndAtomicBoolPriorityMutex Low Priority:     67479548, High Priority:     69340661
   MutexAndTwoBoolPriorityMutex Low Priority:     71057079, High Priority:     73225637
      1000,    1000000,         10
             BasicPriorityMutex Low Priority: 104229260372, High Priority: 104998081670
          TwoMutexPriorityMutex Low Priority:    135207874, High Priority:    142081011
MutexAndAtomicBoolPriorityMutex Low Priority:    123784311, High Priority:    126485813
   MutexAndTwoBoolPriorityMutex Low Priority:    123395465, High Priority:    128321293
      1000,    1000000,        100
             BasicPriorityMutex Low Priority: 105215051929, High Priority: 105995973507
          TwoMutexPriorityMutex Low Priority:    136314321, High Priority:    132271321
MutexAndAtomicBoolPriorityMutex Low Priority:    135147208, High Priority:    129508035
   MutexAndTwoBoolPriorityMutex Low Priority:    135989732, High Priority:    129266694
      1000,    1000000,       1000
             BasicPriorityMutex Low Priority: 106220523228, High Priority: 106983090095
          TwoMutexPriorityMutex Low Priority:    140741159, High Priority:     17110641
MutexAndAtomicBoolPriorityMutex Low Priority:    148747744, High Priority:     23442638
   MutexAndTwoBoolPriorityMutex Low Priority:    143561811, High Priority:     19707176
      1000,    1000000,      10000
             BasicPriorityMutex Low Priority: 107072463797, High Priority: 107733986828
          TwoMutexPriorityMutex Low Priority:   1240355435, High Priority:     47967034
MutexAndAtomicBoolPriorityMutex Low Priority:   1239233628, High Priority:     44028636
   MutexAndTwoBoolPriorityMutex Low Priority:   1245395713, High Priority:     54522443
      1000,    1000000,     100000
             BasicPriorityMutex Low Priority: 102270722197, High Priority: 101196390039
          TwoMutexPriorityMutex Low Priority:  10958942436, High Priority:     69727049
MutexAndAtomicBoolPriorityMutex Low Priority:  10956169106, High Priority:     65150549
   MutexAndTwoBoolPriorityMutex Low Priority:  10954417553, High Priority:     68233233
      1000,    1000000,    1000000
             BasicPriorityMutex Low Priority: 108288981561, High Priority:  96997551785
          TwoMutexPriorityMutex Low Priority:  59973768825, High Priority:     40000053
MutexAndAtomicBoolPriorityMutex Low Priority:  59946746104, High Priority:     35559711
   MutexAndTwoBoolPriorityMutex Low Priority:  59942151521, High Priority:     38588129
     10000,          1,          1
             BasicPriorityMutex Low Priority: 119896264876, High Priority: 120005826424
          TwoMutexPriorityMutex Low Priority: 118036675240, High Priority: 118337478278
MutexAndAtomicBoolPriorityMutex Low Priority: 118052352853, High Priority: 118247228430
   MutexAndTwoBoolPriorityMutex Low Priority: 118010294651, High Priority: 118309321075
     10000,          1,         10
             BasicPriorityMutex Low Priority: 119899501083, High Priority: 120007150620
          TwoMutexPriorityMutex Low Priority: 118062143320, High Priority: 118228853744
MutexAndAtomicBoolPriorityMutex Low Priority: 118243169187, High Priority: 118190997224
   MutexAndTwoBoolPriorityMutex Low Priority: 118003786827, High Priority: 118198365770
     10000,          1,        100
             BasicPriorityMutex Low Priority: 119896179433, High Priority: 120002752492
          TwoMutexPriorityMutex Low Priority: 118016965145, High Priority: 117173073231
MutexAndAtomicBoolPriorityMutex Low Priority: 118070098137, High Priority: 117102414986
   MutexAndTwoBoolPriorityMutex Low Priority: 117992816423, High Priority: 117157956458
     10000,          1,       1000
             BasicPriorityMutex Low Priority: 119901883543, High Priority: 120008958754
          TwoMutexPriorityMutex Low Priority: 118050947507, High Priority: 106071857008
MutexAndAtomicBoolPriorityMutex Low Priority: 118087397428, High Priority: 106027696871
   MutexAndTwoBoolPriorityMutex Low Priority: 117993295359, High Priority: 106032625411
     10000,          1,      10000
             BasicPriorityMutex Low Priority: 119894487098, High Priority: 119944158227
          TwoMutexPriorityMutex Low Priority: 118729207383, High Priority:  18617000528
MutexAndAtomicBoolPriorityMutex Low Priority: 118760524135, High Priority:  17523776238
   MutexAndTwoBoolPriorityMutex Low Priority: 118684411802, High Priority:  19016377202
     10000,          1,     100000
             BasicPriorityMutex Low Priority: 119901560565, High Priority: 119505971933
          TwoMutexPriorityMutex Low Priority: 119783182133, High Priority:   1559995080
MutexAndAtomicBoolPriorityMutex Low Priority: 119787986170, High Priority:   1550854280
   MutexAndTwoBoolPriorityMutex Low Priority: 119787582365, High Priority:   1486435119
     10000,          1,    1000000
             BasicPriorityMutex Low Priority: 119901351826, High Priority: 116007815807
          TwoMutexPriorityMutex Low Priority: 119970435041, High Priority:    426704812
MutexAndAtomicBoolPriorityMutex Low Priority: 119961256988, High Priority:    402087334
   MutexAndTwoBoolPriorityMutex Low Priority: 119959002840, High Priority:    421325517
     10000,         10,          1
             BasicPriorityMutex Low Priority: 119897462427, High Priority: 120007717315
          TwoMutexPriorityMutex Low Priority: 117935687131, High Priority: 118243282996
MutexAndAtomicBoolPriorityMutex Low Priority: 117966010855, High Priority: 118146348874
   MutexAndTwoBoolPriorityMutex Low Priority: 117882772831, High Priority: 118201435514
     10000,         10,         10
             BasicPriorityMutex Low Priority: 119899888306, High Priority: 120009162667
          TwoMutexPriorityMutex Low Priority: 117942816357, High Priority: 118134274269
MutexAndAtomicBoolPriorityMutex Low Priority: 117946823442, High Priority: 118047424015
   MutexAndTwoBoolPriorityMutex Low Priority: 117905967652, High Priority: 118107158698
     10000,         10,        100
             BasicPriorityMutex Low Priority: 119899460458, High Priority: 120007884709
          TwoMutexPriorityMutex Low Priority: 117940033173, High Priority: 117076385184
MutexAndAtomicBoolPriorityMutex Low Priority: 117949966342, High Priority: 117002380905
   MutexAndTwoBoolPriorityMutex Low Priority: 117895414565, High Priority: 117057434785
     10000,         10,       1000
             BasicPriorityMutex Low Priority: 119893001770, High Priority: 119999408802
          TwoMutexPriorityMutex Low Priority: 117926069506, High Priority: 105948171731
MutexAndAtomicBoolPriorityMutex Low Priority: 117951736262, High Priority: 105872982154
   MutexAndTwoBoolPriorityMutex Low Priority: 117906700399, High Priority: 105971952926
     10000,         10,      10000
             BasicPriorityMutex Low Priority: 119897170959, High Priority: 119987226888
          TwoMutexPriorityMutex Low Priority: 118647143979, High Priority:  19009879817
MutexAndAtomicBoolPriorityMutex Low Priority: 118666150701, High Priority:  17690143242
   MutexAndTwoBoolPriorityMutex Low Priority: 118595755546, High Priority:  19013134519
     10000,         10,     100000
             BasicPriorityMutex Low Priority: 119899211591, High Priority: 119607554218
          TwoMutexPriorityMutex Low Priority: 119772690113, High Priority:   1561964661
MutexAndAtomicBoolPriorityMutex Low Priority: 119777232974, High Priority:   1505656258
   MutexAndTwoBoolPriorityMutex Low Priority: 119770743662, High Priority:   1551337932
     10000,         10,    1000000
             BasicPriorityMutex Low Priority: 119903535456, High Priority: 119009618175
          TwoMutexPriorityMutex Low Priority: 119967124287, High Priority:    412965441
MutexAndAtomicBoolPriorityMutex Low Priority: 119954712094, High Priority:    426105767
   MutexAndTwoBoolPriorityMutex Low Priority: 119953216451, High Priority:    427489100
     10000,        100,          1
             BasicPriorityMutex Low Priority: 119894782667, High Priority: 120001890937
          TwoMutexPriorityMutex Low Priority: 116874096811, High Priority: 117189874080
MutexAndAtomicBoolPriorityMutex Low Priority: 116898278163, High Priority: 117091111232
   MutexAndTwoBoolPriorityMutex Low Priority: 116841766817, High Priority: 117150046212
     10000,        100,         10
             BasicPriorityMutex Low Priority: 119892113453, High Priority: 120001285742
          TwoMutexPriorityMutex Low Priority: 116896024128, High Priority: 117091219429
MutexAndAtomicBoolPriorityMutex Low Priority: 116904438404, High Priority: 116998447795
   MutexAndTwoBoolPriorityMutex Low Priority: 116846907568, High Priority: 117059716565
     10000,        100,        100
             BasicPriorityMutex Low Priority: 119895587362, High Priority: 120005303556
          TwoMutexPriorityMutex Low Priority: 116881791243, High Priority: 116048773905
MutexAndAtomicBoolPriorityMutex Low Priority: 116906778162, High Priority: 115974032423
   MutexAndTwoBoolPriorityMutex Low Priority: 116842755551, High Priority: 116026878966
     10000,        100,       1000
             BasicPriorityMutex Low Priority: 119894132455, High Priority: 119999019539
          TwoMutexPriorityMutex Low Priority: 116876518938, High Priority: 105025117665
MutexAndAtomicBoolPriorityMutex Low Priority: 116920605681, High Priority: 104955902386
   MutexAndTwoBoolPriorityMutex Low Priority: 116840410933, High Priority: 105022332011
     10000,        100,      10000
             BasicPriorityMutex Low Priority: 119898716972, High Priority: 119967581359
          TwoMutexPriorityMutex Low Priority: 117754217559, High Priority:  18141938887
MutexAndAtomicBoolPriorityMutex Low Priority: 117783062849, High Priority:  18248631659
   MutexAndTwoBoolPriorityMutex Low Priority: 117693327773, High Priority:  17877409785
     10000,        100,     100000
             BasicPriorityMutex Low Priority: 119892166696, High Priority: 119800257805
          TwoMutexPriorityMutex Low Priority: 119662521424, High Priority:   1569844767
MutexAndAtomicBoolPriorityMutex Low Priority: 119663532602, High Priority:   1557198157
   MutexAndTwoBoolPriorityMutex Low Priority: 119663470220, High Priority:   1573875599
     10000,        100,    1000000
             BasicPriorityMutex Low Priority: 119900936580, High Priority: 118005474593
          TwoMutexPriorityMutex Low Priority: 119951086660, High Priority:    438134757
MutexAndAtomicBoolPriorityMutex Low Priority: 119945892169, High Priority:    427427241
   MutexAndTwoBoolPriorityMutex Low Priority: 119944956896, High Priority:    418463792
     10000,       1000,          1
             BasicPriorityMutex Low Priority: 119889684150, High Priority: 119999274873
          TwoMutexPriorityMutex Low Priority: 106647391417, High Priority: 107363172034
MutexAndAtomicBoolPriorityMutex Low Priority: 101271641612, High Priority: 101740418591
   MutexAndTwoBoolPriorityMutex Low Priority: 100009866209, High Priority: 100587559292
     10000,       1000,         10
             BasicPriorityMutex Low Priority: 119898019913, High Priority: 120005946545
          TwoMutexPriorityMutex Low Priority: 106642895342, High Priority: 107268665350
MutexAndAtomicBoolPriorityMutex Low Priority: 106215322610, High Priority: 106687705481
   MutexAndTwoBoolPriorityMutex Low Priority: 106212280283, High Priority: 106797543609
     10000,       1000,        100
             BasicPriorityMutex Low Priority: 119898292913, High Priority: 120006829489
          TwoMutexPriorityMutex Low Priority: 106646835782, High Priority: 106315964171
MutexAndAtomicBoolPriorityMutex Low Priority: 106706122442, High Priority: 106257801210
   MutexAndTwoBoolPriorityMutex Low Priority: 106633312142, High Priority: 106302959241
     10000,       1000,       1000
             BasicPriorityMutex Low Priority: 119897577404, High Priority: 120004238027
          TwoMutexPriorityMutex Low Priority: 106639139044, High Priority:  96263839178
MutexAndAtomicBoolPriorityMutex Low Priority: 106731373883, High Priority:  96214230636
   MutexAndTwoBoolPriorityMutex Low Priority: 106616980987, High Priority:  96235571478
     10000,       1000,      10000
             BasicPriorityMutex Low Priority: 119900124968, High Priority: 119998902224
          TwoMutexPriorityMutex Low Priority: 107915408850, High Priority:   8795924891
MutexAndAtomicBoolPriorityMutex Low Priority: 108436931938, High Priority:  11497542639
   MutexAndTwoBoolPriorityMutex Low Priority: 107976404214, High Priority:   8940717975
     10000,       1000,     100000
             BasicPriorityMutex Low Priority: 119890148622, High Priority: 119799167387
          TwoMutexPriorityMutex Low Priority: 118502750849, High Priority:   1593973086
MutexAndAtomicBoolPriorityMutex Low Priority: 118503100281, High Priority:   1577483018
   MutexAndTwoBoolPriorityMutex Low Priority: 118513431042, High Priority:   1579888110
     10000,       1000,    1000000
             BasicPriorityMutex Low Priority: 119899853071, High Priority: 117006568318
          TwoMutexPriorityMutex Low Priority: 119830408185, High Priority:    415416991
MutexAndAtomicBoolPriorityMutex Low Priority: 119833289712, High Priority:    410743094
   MutexAndTwoBoolPriorityMutex Low Priority: 119823723743, High Priority:    434690187
     10000,      10000,          1
             BasicPriorityMutex Low Priority: 119884033241, High Priority: 119990325977
          TwoMutexPriorityMutex Low Priority:  59569386104, High Priority:  59993281170
MutexAndAtomicBoolPriorityMutex Low Priority:  46773873990, High Priority:  47003506510
   MutexAndTwoBoolPriorityMutex Low Priority:  45537481061, High Priority:  45783578243
     10000,      10000,         10
             BasicPriorityMutex Low Priority: 119897718843, High Priority: 120005617127
          TwoMutexPriorityMutex Low Priority:  59573854607, High Priority:  59922355060
MutexAndAtomicBoolPriorityMutex Low Priority:  57778688321, High Priority:  58038450667
   MutexAndTwoBoolPriorityMutex Low Priority:  57990929587, High Priority:  58331702190
     10000,      10000,        100
             BasicPriorityMutex Low Priority: 119863553838, High Priority: 119971204776
          TwoMutexPriorityMutex Low Priority:  59559698454, High Priority:  59396999692
MutexAndAtomicBoolPriorityMutex Low Priority:  59591950977, High Priority:  59347992057
   MutexAndTwoBoolPriorityMutex Low Priority:  59569236674, High Priority:  59386310489
     10000,      10000,       1000
             BasicPriorityMutex Low Priority: 119893263075, High Priority: 119999912176
          TwoMutexPriorityMutex Low Priority:  59568913259, High Priority:  53771310549
MutexAndAtomicBoolPriorityMutex Low Priority:  59602234234, High Priority:  53719888299
   MutexAndTwoBoolPriorityMutex Low Priority:  59564797382, High Priority:  53764633575
     10000,      10000,      10000
             BasicPriorityMutex Low Priority: 119859047369, High Priority: 119917074654
          TwoMutexPriorityMutex Low Priority:  61454936495, High Priority:   4054046367
MutexAndAtomicBoolPriorityMutex Low Priority:  62093108233, High Priority:   5183011676
   MutexAndTwoBoolPriorityMutex Low Priority:  61658929724, High Priority:   4456088780
     10000,      10000,     100000
             BasicPriorityMutex Low Priority: 119888050232, High Priority: 119795429359
          TwoMutexPriorityMutex Low Priority: 108947906784, High Priority:   1442983329
MutexAndAtomicBoolPriorityMutex Low Priority: 108951423886, High Priority:   1476037506
   MutexAndTwoBoolPriorityMutex Low Priority: 108947372055, High Priority:   1461577671
     10000,      10000,    1000000
             BasicPriorityMutex Low Priority: 119890185550, High Priority: 117997315848
          TwoMutexPriorityMutex Low Priority: 118769961757, High Priority:    413808276
MutexAndAtomicBoolPriorityMutex Low Priority: 118764823841, High Priority:    411938833
   MutexAndTwoBoolPriorityMutex Low Priority: 118769092658, High Priority:    410180738
     10000,     100000,          1
             BasicPriorityMutex Low Priority: 119295305956, High Priority: 119399650294
          TwoMutexPriorityMutex Low Priority:  11025110092, High Priority:  11096100625
MutexAndAtomicBoolPriorityMutex Low Priority:   6389121576, High Priority:   6405174311
   MutexAndTwoBoolPriorityMutex Low Priority:   6929797373, High Priority:   6957434801
     10000,     100000,         10
             BasicPriorityMutex Low Priority: 119694626665, High Priority: 119804280974
          TwoMutexPriorityMutex Low Priority:  11028733884, High Priority:  11091539871
MutexAndAtomicBoolPriorityMutex Low Priority:  10173772222, High Priority:  10209023294
   MutexAndTwoBoolPriorityMutex Low Priority:  10151871626, High Priority:  10200479687
     10000,     100000,        100
             BasicPriorityMutex Low Priority: 119597883726, High Priority: 119705227001
          TwoMutexPriorityMutex Low Priority:  11024164882, High Priority:  10987115553
MutexAndAtomicBoolPriorityMutex Low Priority:  11022346809, High Priority:  10970129154
   MutexAndTwoBoolPriorityMutex Low Priority:  11022017748, High Priority:  10974070431
     10000,     100000,       1000
             BasicPriorityMutex Low Priority: 119792332402, High Priority: 119898905807
          TwoMutexPriorityMutex Low Priority:  11025808232, High Priority:   9947534825
MutexAndAtomicBoolPriorityMutex Low Priority:  11025697658, High Priority:   9926823855
   MutexAndTwoBoolPriorityMutex Low Priority:  11022042611, High Priority:   9942619739
     10000,     100000,      10000
             BasicPriorityMutex Low Priority: 119808524199, High Priority: 119887911151
          TwoMutexPriorityMutex Low Priority:  11647591268, High Priority:    777587121
MutexAndAtomicBoolPriorityMutex Low Priority:  11648138185, High Priority:    754247029
   MutexAndTwoBoolPriorityMutex Low Priority:  11768875775, High Priority:    923152688
     10000,     100000,     100000
             BasicPriorityMutex Low Priority: 119697607151, High Priority: 119505523277
          TwoMutexPriorityMutex Low Priority:  60324274518, High Priority:    818738823
MutexAndAtomicBoolPriorityMutex Low Priority:  60322939851, High Priority:    802678882
   MutexAndTwoBoolPriorityMutex Low Priority:  60326481128, High Priority:    809663264
     10000,     100000,    1000000
             BasicPriorityMutex Low Priority: 119799853470, High Priority: 117906953184
          TwoMutexPriorityMutex Low Priority: 109153559696, High Priority:    383052142
MutexAndAtomicBoolPriorityMutex Low Priority: 109155266294, High Priority:    379692333
   MutexAndTwoBoolPriorityMutex Low Priority: 109144474464, High Priority:    380518647
     10000,    1000000,          1
             BasicPriorityMutex Low Priority: 117894967360, High Priority: 118000436132
          TwoMutexPriorityMutex Low Priority:   1215851255, High Priority:   1215430728
MutexAndAtomicBoolPriorityMutex Low Priority:    576796944, High Priority:    568261355
   MutexAndTwoBoolPriorityMutex Low Priority:    516077637, High Priority:    508844120
     10000,    1000000,         10
             BasicPriorityMutex Low Priority: 117895286790, High Priority: 118001396732
          TwoMutexPriorityMutex Low Priority:   1216354642, High Priority:   1215165963
MutexAndAtomicBoolPriorityMutex Low Priority:   1125009538, High Priority:   1120105501
   MutexAndTwoBoolPriorityMutex Low Priority:   1155223057, High Priority:   1150620672
     10000,    1000000,        100
             BasicPriorityMutex Low Priority: 116895865714, High Priority: 117001457828
          TwoMutexPriorityMutex Low Priority:   1216311887, High Priority:   1203842386
MutexAndAtomicBoolPriorityMutex Low Priority:   1216378123, High Priority:   1202097468
   MutexAndTwoBoolPriorityMutex Low Priority:   1214629809, High Priority:   1201860330
     10000,    1000000,       1000
             BasicPriorityMutex Low Priority: 116895078837, High Priority: 116997154935
          TwoMutexPriorityMutex Low Priority:   1215928103, High Priority:   1089599231
MutexAndAtomicBoolPriorityMutex Low Priority:   1216160944, High Priority:   1087875127
   MutexAndTwoBoolPriorityMutex Low Priority:   1215645355, High Priority:   1090324475
     10000,    1000000,      10000
             BasicPriorityMutex Low Priority: 118892496116, High Priority: 118980183661
          TwoMutexPriorityMutex Low Priority:   1266246588, High Priority:     62631514
MutexAndAtomicBoolPriorityMutex Low Priority:   1276580964, High Priority:     72985933
   MutexAndTwoBoolPriorityMutex Low Priority:   1387299496, High Priority:    184233014
     10000,    1000000,     100000
             BasicPriorityMutex Low Priority: 117895725815, High Priority: 117702452742
          TwoMutexPriorityMutex Low Priority:  11054979778, High Priority:    147751108
MutexAndAtomicBoolPriorityMutex Low Priority:  11056783054, High Priority:    149768038
   MutexAndTwoBoolPriorityMutex Low Priority:  11057269425, High Priority:    150080523
     10000,    1000000,    1000000
             BasicPriorityMutex Low Priority: 119898514306, High Priority: 119004740071
          TwoMutexPriorityMutex Low Priority:  60236600297, High Priority:    235642511
MutexAndAtomicBoolPriorityMutex Low Priority:  60216171802, High Priority:    218779480
   MutexAndTwoBoolPriorityMutex Low Priority:  60198371754, High Priority:    202796206
    100000,          1,          1
             BasicPriorityMutex Low Priority: 120064618454, High Priority: 120077790349
          TwoMutexPriorityMutex Low Priority: 119861490535, High Priority: 119897378335
MutexAndAtomicBoolPriorityMutex Low Priority: 119862315554, High Priority: 119888053558
   MutexAndTwoBoolPriorityMutex Low Priority: 119859481037, High Priority: 119893802994
    100000,          1,         10
             BasicPriorityMutex Low Priority: 120062755120, High Priority: 120075695712
          TwoMutexPriorityMutex Low Priority: 119857798830, High Priority: 119885627324
MutexAndAtomicBoolPriorityMutex Low Priority: 119863357409, High Priority: 119870856169
   MutexAndTwoBoolPriorityMutex Low Priority: 119861448287, High Priority: 119883953784
    100000,          1,        100
             BasicPriorityMutex Low Priority: 120062958509, High Priority: 120075761087
          TwoMutexPriorityMutex Low Priority: 119863077237, High Priority: 119776992083
MutexAndAtomicBoolPriorityMutex Low Priority: 119862619112, High Priority: 119768288691
   MutexAndTwoBoolPriorityMutex Low Priority: 119861673637, High Priority: 119784161121
    100000,          1,       1000
             BasicPriorityMutex Low Priority: 120066285693, High Priority: 120078286077
          TwoMutexPriorityMutex Low Priority: 119864027275, High Priority: 118634444212
MutexAndAtomicBoolPriorityMutex Low Priority: 119858450033, High Priority: 118622625676
   MutexAndTwoBoolPriorityMutex Low Priority: 119860879156, High Priority: 118640689042
    100000,          1,      10000
             BasicPriorityMutex Low Priority: 120064170463, High Priority: 120066837909
          TwoMutexPriorityMutex Low Priority: 119841968870, High Priority: 107830472431
MutexAndAtomicBoolPriorityMutex Low Priority: 119840943252, High Priority: 107817007075
   MutexAndTwoBoolPriorityMutex Low Priority: 119859849157, High Priority: 107846786490
    100000,          1,     100000
             BasicPriorityMutex Low Priority: 120068184044, High Priority: 119980955912
          TwoMutexPriorityMutex Low Priority: 119864869625, High Priority:  16878509928
MutexAndAtomicBoolPriorityMutex Low Priority: 119962386383, High Priority:  16169179991
   MutexAndTwoBoolPriorityMutex Low Priority: 119867960274, High Priority:  16882526857
    100000,          1,    1000000
             BasicPriorityMutex Low Priority: 120061410196, High Priority: 119074524679
          TwoMutexPriorityMutex Low Priority: 120065534737, High Priority:    163239432
MutexAndAtomicBoolPriorityMutex Low Priority: 120059773127, High Priority:    156355219
   MutexAndTwoBoolPriorityMutex Low Priority: 120060091773, High Priority:    158449193
    100000,         10,          1
             BasicPriorityMutex Low Priority: 120063984062, High Priority: 120077114122
          TwoMutexPriorityMutex Low Priority: 119864822804, High Priority: 119900784781
MutexAndAtomicBoolPriorityMutex Low Priority: 119864324726, High Priority: 119891330367
   MutexAndTwoBoolPriorityMutex Low Priority: 119863990066, High Priority: 119902411938
    100000,         10,         10
             BasicPriorityMutex Low Priority: 120065227172, High Priority: 120078007255
          TwoMutexPriorityMutex Low Priority: 119859441355, High Priority: 119878373608
MutexAndAtomicBoolPriorityMutex Low Priority: 119863856976, High Priority: 119877202392
   MutexAndTwoBoolPriorityMutex Low Priority: 119864269794, High Priority: 119893825702
    100000,         10,        100
             BasicPriorityMutex Low Priority: 120063994154, High Priority: 120076744960
          TwoMutexPriorityMutex Low Priority: 119867961020, High Priority: 119780135197
MutexAndAtomicBoolPriorityMutex Low Priority: 119865073390, High Priority: 119775251029
   MutexAndTwoBoolPriorityMutex Low Priority: 119863049295, High Priority: 119786072321
    100000,         10,       1000
             BasicPriorityMutex Low Priority: 120066752541, High Priority: 120078348481
          TwoMutexPriorityMutex Low Priority: 119863048340, High Priority: 118635942537
MutexAndAtomicBoolPriorityMutex Low Priority: 119863998631, High Priority: 118624097649
   MutexAndTwoBoolPriorityMutex Low Priority: 119869417249, High Priority: 118647172263
    100000,         10,      10000
             BasicPriorityMutex Low Priority: 120047045516, High Priority: 120050471568
          TwoMutexPriorityMutex Low Priority: 119864588856, High Priority: 107845090038
MutexAndAtomicBoolPriorityMutex Low Priority: 119860622665, High Priority: 107828408005
   MutexAndTwoBoolPriorityMutex Low Priority: 119865269800, High Priority: 107847993425
    100000,         10,     100000
             BasicPriorityMutex Low Priority: 120060823214, High Priority: 119973877686
          TwoMutexPriorityMutex Low Priority: 119868982119, High Priority:  15179353232
MutexAndAtomicBoolPriorityMutex Low Priority: 119958692275, High Priority:  14062146815
   MutexAndTwoBoolPriorityMutex Low Priority: 119858405137, High Priority:  18579900174
    100000,         10,    1000000
             BasicPriorityMutex Low Priority: 120057723938, High Priority: 119070619931
          TwoMutexPriorityMutex Low Priority: 120043510692, High Priority:    139014708
MutexAndAtomicBoolPriorityMutex Low Priority: 120039975469, High Priority:    136150989
   MutexAndTwoBoolPriorityMutex Low Priority: 120041056027, High Priority:    137512585
    100000,        100,          1
             BasicPriorityMutex Low Priority: 120043950538, High Priority: 120057391633
          TwoMutexPriorityMutex Low Priority: 119765723448, High Priority: 119806288358
MutexAndAtomicBoolPriorityMutex Low Priority: 119765483802, High Priority: 119792729726
   MutexAndTwoBoolPriorityMutex Low Priority: 119761568264, High Priority: 119798053430
    100000,        100,         10
             BasicPriorityMutex Low Priority: 120068082445, High Priority: 120081057729
          TwoMutexPriorityMutex Low Priority: 119764932184, High Priority: 119790481003
MutexAndAtomicBoolPriorityMutex Low Priority: 119765081524, High Priority: 119781149131
   MutexAndTwoBoolPriorityMutex Low Priority: 119758495953, High Priority: 119783560799
    100000,        100,        100
             BasicPriorityMutex Low Priority: 120062816913, High Priority: 120075271960
          TwoMutexPriorityMutex Low Priority: 119767079983, High Priority: 119685556284
MutexAndAtomicBoolPriorityMutex Low Priority: 119762350031, High Priority: 119668523190
   MutexAndTwoBoolPriorityMutex Low Priority: 119664064729, High Priority: 119587785431
    100000,        100,       1000
             BasicPriorityMutex Low Priority: 120067395392, High Priority: 120079088356
          TwoMutexPriorityMutex Low Priority: 119762564571, High Priority: 118539432489
MutexAndAtomicBoolPriorityMutex Low Priority: 119762620085, High Priority: 118526429484
   MutexAndTwoBoolPriorityMutex Low Priority: 119665629672, High Priority: 118441572995
    100000,        100,      10000
             BasicPriorityMutex Low Priority: 120060188811, High Priority: 120063085374
          TwoMutexPriorityMutex Low Priority: 119760804867, High Priority: 107754671251
MutexAndAtomicBoolPriorityMutex Low Priority: 119764331004, High Priority: 107749478749
   MutexAndTwoBoolPriorityMutex Low Priority: 119763822719, High Priority: 107765371524
    100000,        100,     100000
             BasicPriorityMutex Low Priority: 120065031207, High Priority: 119877792487
          TwoMutexPriorityMutex Low Priority: 119765300001, High Priority:  17676206096
MutexAndAtomicBoolPriorityMutex Low Priority: 119863589403, High Priority:  15568496950
   MutexAndTwoBoolPriorityMutex Low Priority: 119860538598, High Priority:  17780255873
    100000,        100,    1000000
             BasicPriorityMutex Low Priority: 120066161549, High Priority: 119079042064
          TwoMutexPriorityMutex Low Priority: 120062351177, High Priority:    160207850
MutexAndAtomicBoolPriorityMutex Low Priority: 120060375668, High Priority:    156871967
   MutexAndTwoBoolPriorityMutex Low Priority: 120062562811, High Priority:    160231120
    100000,       1000,          1
             BasicPriorityMutex Low Priority: 120061414000, High Priority: 120073846294
          TwoMutexPriorityMutex Low Priority: 118558238642, High Priority: 118638279347
MutexAndAtomicBoolPriorityMutex Low Priority: 117952406130, High Priority: 118004433263
   MutexAndTwoBoolPriorityMutex Low Priority: 117555632240, High Priority: 117627152374
    100000,       1000,         10
             BasicPriorityMutex Low Priority: 120068884242, High Priority: 120081538898
          TwoMutexPriorityMutex Low Priority: 118560499405, High Priority: 118636434390
MutexAndAtomicBoolPriorityMutex Low Priority: 118461154917, High Priority: 118522279924
   MutexAndTwoBoolPriorityMutex Low Priority: 118562516333, High Priority: 118626601677
    100000,       1000,        100
             BasicPriorityMutex Low Priority: 120067521311, High Priority: 120079882022
          TwoMutexPriorityMutex Low Priority: 118555314032, High Priority: 118516224299
MutexAndAtomicBoolPriorityMutex Low Priority: 118555556839, High Priority: 118502124872
   MutexAndTwoBoolPriorityMutex Low Priority: 118561069555, High Priority: 118519326470
    100000,       1000,       1000
             BasicPriorityMutex Low Priority: 120068032111, High Priority: 120079747579
          TwoMutexPriorityMutex Low Priority: 118563993214, High Priority: 117400438440
MutexAndAtomicBoolPriorityMutex Low Priority: 118562586696, High Priority: 117380737261
   MutexAndTwoBoolPriorityMutex Low Priority: 118562962373, High Priority: 117403753422
    100000,       1000,      10000
             BasicPriorityMutex Low Priority: 120062457255, High Priority: 120064873262
          TwoMutexPriorityMutex Low Priority: 118561355301, High Priority: 106726818973
MutexAndAtomicBoolPriorityMutex Low Priority: 118561367347, High Priority: 106708730305
   MutexAndTwoBoolPriorityMutex Low Priority: 118561388700, High Priority: 106722606981
    100000,       1000,     100000
             BasicPriorityMutex Low Priority: 120064352380, High Priority: 119976881585
          TwoMutexPriorityMutex Low Priority: 118751942537, High Priority:   9514736056
MutexAndAtomicBoolPriorityMutex Low Priority: 118756024180, High Priority:   9303180296
   MutexAndTwoBoolPriorityMutex Low Priority: 118658141124, High Priority:   8013061005
    100000,       1000,    1000000
             BasicPriorityMutex Low Priority: 120070562784, High Priority: 119083639735
          TwoMutexPriorityMutex Low Priority: 119862098276, High Priority:    165631883
MutexAndAtomicBoolPriorityMutex Low Priority: 119942537186, High Priority:    142584523
   MutexAndTwoBoolPriorityMutex Low Priority: 119861844326, High Priority:    164975800
    100000,      10000,          1
             BasicPriorityMutex Low Priority: 120061507775, High Priority: 120074198434
          TwoMutexPriorityMutex Low Priority: 108850492473, High Priority: 108935437867
MutexAndAtomicBoolPriorityMutex Low Priority: 103638325191, High Priority: 103692845560
   MutexAndTwoBoolPriorityMutex Low Priority: 101333765471, High Priority: 101297283021
    100000,      10000,         10
             BasicPriorityMutex Low Priority: 120058839568, High Priority: 120071920492
          TwoMutexPriorityMutex Low Priority: 108848291281, High Priority: 108923350578
MutexAndAtomicBoolPriorityMutex Low Priority: 108249957294, High Priority: 108309342571
   MutexAndTwoBoolPriorityMutex Low Priority: 108343735670, High Priority: 108400388475
    100000,      10000,        100
             BasicPriorityMutex Low Priority: 120060619987, High Priority: 120073534259
          TwoMutexPriorityMutex Low Priority: 108847925138, High Priority: 108812375668
MutexAndAtomicBoolPriorityMutex Low Priority: 108847131603, High Priority: 108805484962
   MutexAndTwoBoolPriorityMutex Low Priority: 108843246758, High Priority: 108810576715
    100000,      10000,       1000
             BasicPriorityMutex Low Priority: 120058631254, High Priority: 120070432148
          TwoMutexPriorityMutex Low Priority: 108847748734, High Priority: 107786405480
MutexAndAtomicBoolPriorityMutex Low Priority: 108946868945, High Priority: 107762776173
   MutexAndTwoBoolPriorityMutex Low Priority: 108851496496, High Priority: 107790049404
    100000,      10000,      10000
             BasicPriorityMutex Low Priority: 120061453044, High Priority: 120054296302
          TwoMutexPriorityMutex Low Priority: 108847010897, High Priority:  97990550998
MutexAndAtomicBoolPriorityMutex Low Priority: 108844885871, High Priority:  97967177573
   MutexAndTwoBoolPriorityMutex Low Priority: 108850447230, High Priority:  97991988802
    100000,      10000,     100000
             BasicPriorityMutex Low Priority: 120060072834, High Priority: 119872727330
          TwoMutexPriorityMutex Low Priority: 109644644247, High Priority:   7706112821
MutexAndAtomicBoolPriorityMutex Low Priority: 109644716095, High Priority:   8095205245
   MutexAndTwoBoolPriorityMutex Low Priority: 109844730747, High Priority:   9604980434
    100000,      10000,    1000000
             BasicPriorityMutex Low Priority: 120065563572, High Priority: 119078580178
          TwoMutexPriorityMutex Low Priority: 118868448845, High Priority:    169872263
MutexAndAtomicBoolPriorityMutex Low Priority: 118870669259, High Priority:    172082692
   MutexAndTwoBoolPriorityMutex Low Priority: 118868517153, High Priority:    171578534
    100000,     100000,          1
             BasicPriorityMutex Low Priority: 120063785339, High Priority: 120076398823
          TwoMutexPriorityMutex Low Priority:  60081182111, High Priority:  60024804204
MutexAndAtomicBoolPriorityMutex Low Priority:  40149848145, High Priority:  40167410938
   MutexAndTwoBoolPriorityMutex Low Priority:  45560605716, High Priority:  45487959996
    100000,     100000,         10
             BasicPriorityMutex Low Priority: 120063150673, High Priority: 120075767041
          TwoMutexPriorityMutex Low Priority:  60084055182, High Priority:  60027937356
MutexAndAtomicBoolPriorityMutex Low Priority:  57482342687, High Priority:  57411753740
   MutexAndTwoBoolPriorityMutex Low Priority:  58280004442, High Priority:  58217933282
    100000,     100000,        100
             BasicPriorityMutex Low Priority: 120065245671, High Priority: 120078243493
          TwoMutexPriorityMutex Low Priority:  60083955231, High Priority:  59978161367
MutexAndAtomicBoolPriorityMutex Low Priority:  59882456732, High Priority:  59860877326
   MutexAndTwoBoolPriorityMutex Low Priority:  60081358352, High Priority:  59962430241
    100000,     100000,       1000
             BasicPriorityMutex Low Priority: 120064120406, High Priority: 120075887242
          TwoMutexPriorityMutex Low Priority:  60079258272, High Priority:  59393642307
MutexAndAtomicBoolPriorityMutex Low Priority:  60083086598, High Priority:  59388961629
   MutexAndTwoBoolPriorityMutex Low Priority:  60082666134, High Priority:  59395820776
    100000,     100000,      10000
             BasicPriorityMutex Low Priority: 120055252314, High Priority: 120057703468
          TwoMutexPriorityMutex Low Priority:  60081347736, High Priority:  54002526154
MutexAndAtomicBoolPriorityMutex Low Priority:  60081270047, High Priority:  53989989001
   MutexAndTwoBoolPriorityMutex Low Priority:  60082342332, High Priority:  54004240732
    100000,     100000,     100000
             BasicPriorityMutex Low Priority: 120061871723, High Priority: 119974101568
          TwoMutexPriorityMutex Low Priority:  61981367301, High Priority:   3957365907
MutexAndAtomicBoolPriorityMutex Low Priority:  61983257283, High Priority:   3960861023
   MutexAndTwoBoolPriorityMutex Low Priority:  62482044327, High Priority:   4961410856
    100000,     100000,    1000000
             BasicPriorityMutex Low Priority: 120059257436, High Priority: 119071905632
          TwoMutexPriorityMutex Low Priority: 109249507634, High Priority:    153786711
MutexAndAtomicBoolPriorityMutex Low Priority: 109250447012, High Priority:    154451001
   MutexAndTwoBoolPriorityMutex Low Priority: 109248353659, High Priority:    152799952
    100000,    1000000,          1
             BasicPriorityMutex Low Priority: 120065292406, High Priority: 120078448621
          TwoMutexPriorityMutex Low Priority:  11014861716, High Priority:  11023426687
MutexAndAtomicBoolPriorityMutex Low Priority:   6408710657, High Priority:   6312276004
   MutexAndTwoBoolPriorityMutex Low Priority:   6108190444, High Priority:   6012505278
    100000,    1000000,         10
             BasicPriorityMutex Low Priority: 120057788754, High Priority: 120070496053
          TwoMutexPriorityMutex Low Priority:  11015287111, High Priority:  11023612081
MutexAndAtomicBoolPriorityMutex Low Priority:  10013987890, High Priority:  10019511311
   MutexAndTwoBoolPriorityMutex Low Priority:  10614384122, High Priority:  10521282805
    100000,    1000000,        100
             BasicPriorityMutex Low Priority: 120066314416, High Priority: 120078855021
          TwoMutexPriorityMutex Low Priority:  11015297594, High Priority:  11012820716
MutexAndAtomicBoolPriorityMutex Low Priority:  11016018301, High Priority:  11009958074
   MutexAndTwoBoolPriorityMutex Low Priority:  11014931039, High Priority:  11012512695
    100000,    1000000,       1000
             BasicPriorityMutex Low Priority: 120060056348, High Priority: 120071797528
          TwoMutexPriorityMutex Low Priority:  11015579311, High Priority:  10909050558
MutexAndAtomicBoolPriorityMutex Low Priority:  11015462252, High Priority:  10905689480
   MutexAndTwoBoolPriorityMutex Low Priority:  11013881224, High Priority:  10905994539
    100000,    1000000,      10000
             BasicPriorityMutex Low Priority: 120060568078, High Priority: 120063405461
          TwoMutexPriorityMutex Low Priority:  11014971250, High Priority:   9916974408
MutexAndAtomicBoolPriorityMutex Low Priority:  11015682374, High Priority:   9916308581
   MutexAndTwoBoolPriorityMutex Low Priority:  11015923283, High Priority:   9917280201
    100000,    1000000,     100000
             BasicPriorityMutex Low Priority: 120069117455, High Priority: 119981989717
          TwoMutexPriorityMutex Low Priority:  11416191752, High Priority:    412821537
MutexAndAtomicBoolPriorityMutex Low Priority:  11815897547, High Priority:    810128826
   MutexAndTwoBoolPriorityMutex Low Priority:  11915782142, High Priority:    912700534
    100000,    1000000,    1000000
             BasicPriorityMutex Low Priority: 120069752706, High Priority: 119082622538
          TwoMutexPriorityMutex Low Priority:  60181937580, High Priority:     84436030
MutexAndAtomicBoolPriorityMutex Low Priority:  60179016132, High Priority:     79470268
   MutexAndTwoBoolPriorityMutex Low Priority:  60182714833, High Priority:     85030993
   1000000,          1,          1
             BasicPriorityMutex Low Priority: 120015780994, High Priority: 120017050321
          TwoMutexPriorityMutex Low Priority: 120016873172, High Priority: 120020783034
MutexAndAtomicBoolPriorityMutex Low Priority: 120016454241, High Priority: 120019629012
   MutexAndTwoBoolPriorityMutex Low Priority: 120016760294, High Priority: 120019337449
   1000000,          1,         10
             BasicPriorityMutex Low Priority: 120016714335, High Priority: 120017891952
          TwoMutexPriorityMutex Low Priority: 120015601176, High Priority: 120016907813
MutexAndAtomicBoolPriorityMutex Low Priority: 120017825978, High Priority: 120019791132
   MutexAndTwoBoolPriorityMutex Low Priority: 120017898144, High Priority: 120021753464
   1000000,          1,        100
             BasicPriorityMutex Low Priority: 120016398393, High Priority: 120017536626
          TwoMutexPriorityMutex Low Priority: 120017629409, High Priority: 120010138189
MutexAndAtomicBoolPriorityMutex Low Priority: 120017863257, High Priority: 120008993206
   MutexAndTwoBoolPriorityMutex Low Priority: 120015402880, High Priority: 120007488569
   1000000,          1,       1000
             BasicPriorityMutex Low Priority: 120016793760, High Priority: 120016874711
          TwoMutexPriorityMutex Low Priority: 120016279998, High Priority: 119894367265
MutexAndAtomicBoolPriorityMutex Low Priority: 120016947340, High Priority: 119894020983
   MutexAndTwoBoolPriorityMutex Low Priority: 120017329706, High Priority: 119895183724
   1000000,          1,      10000
             BasicPriorityMutex Low Priority: 120016118326, High Priority: 120007348142
          TwoMutexPriorityMutex Low Priority: 120015057399, High Priority: 118810435277
MutexAndAtomicBoolPriorityMutex Low Priority: 120014720585, High Priority: 118810403239
   MutexAndTwoBoolPriorityMutex Low Priority: 120015659388, High Priority: 118812465079
   1000000,          1,     100000
             BasicPriorityMutex Low Priority: 120015224285, High Priority: 119916461228
          TwoMutexPriorityMutex Low Priority: 120016022904, High Priority: 108011084190
MutexAndAtomicBoolPriorityMutex Low Priority: 120016258283, High Priority: 108010626693
   MutexAndTwoBoolPriorityMutex Low Priority: 120016064751, High Priority: 108012162249
   1000000,          1,    1000000
             BasicPriorityMutex Low Priority: 120016308851, High Priority: 119017533090
          TwoMutexPriorityMutex Low Priority: 120015783403, High Priority:  18006857668
MutexAndAtomicBoolPriorityMutex Low Priority: 120016836500, High Priority:  12007064780
   MutexAndTwoBoolPriorityMutex Low Priority: 120016621164, High Priority:  22009083312
   1000000,         10,          1
             BasicPriorityMutex Low Priority: 120016376408, High Priority: 120017637033
          TwoMutexPriorityMutex Low Priority: 120016730174, High Priority: 120021287581
MutexAndAtomicBoolPriorityMutex Low Priority: 120016201617, High Priority: 120018815026
   MutexAndTwoBoolPriorityMutex Low Priority: 120015330460, High Priority: 120020204830
   1000000,         10,         10
             BasicPriorityMutex Low Priority: 120015326765, High Priority: 120016575512
          TwoMutexPriorityMutex Low Priority: 120016369849, High Priority: 120019031683
MutexAndAtomicBoolPriorityMutex Low Priority: 120017508348, High Priority: 120020075667
   MutexAndTwoBoolPriorityMutex Low Priority: 120016204305, High Priority: 120019321105
   1000000,         10,        100
             BasicPriorityMutex Low Priority: 120015104063, High Priority: 120016258354
          TwoMutexPriorityMutex Low Priority: 120016242104, High Priority: 120008629691
MutexAndAtomicBoolPriorityMutex Low Priority: 120016236463, High Priority: 120006600401
   MutexAndTwoBoolPriorityMutex Low Priority: 120018025652, High Priority: 120010608653
   1000000,         10,       1000
             BasicPriorityMutex Low Priority: 120015778982, High Priority: 120016052955
          TwoMutexPriorityMutex Low Priority: 120016937283, High Priority: 119893620971
MutexAndAtomicBoolPriorityMutex Low Priority: 120016367701, High Priority: 119893359498
   MutexAndTwoBoolPriorityMutex Low Priority: 120016636738, High Priority: 119896162695
   1000000,         10,      10000
             BasicPriorityMutex Low Priority: 120016191122, High Priority: 120007321871
          TwoMutexPriorityMutex Low Priority: 120016059584, High Priority: 118811766308
MutexAndAtomicBoolPriorityMutex Low Priority: 120015948512, High Priority: 118811290449
   MutexAndTwoBoolPriorityMutex Low Priority: 120015337084, High Priority: 118811364735
   1000000,         10,     100000
             BasicPriorityMutex Low Priority: 120015876648, High Priority: 119917120140
          TwoMutexPriorityMutex Low Priority: 120016068387, High Priority: 108012037388
MutexAndAtomicBoolPriorityMutex Low Priority: 120016409797, High Priority: 108009527352
   MutexAndTwoBoolPriorityMutex Low Priority: 120016685070, High Priority: 108011822944
   1000000,         10,    1000000
             BasicPriorityMutex Low Priority: 120016644700, High Priority: 119017882532
          TwoMutexPriorityMutex Low Priority: 120015674423, High Priority:  22008147535
MutexAndAtomicBoolPriorityMutex Low Priority: 120017281232, High Priority:  17007233314
   MutexAndTwoBoolPriorityMutex Low Priority: 120015036912, High Priority:  18008330412
   1000000,        100,          1
             BasicPriorityMutex Low Priority: 120016862675, High Priority: 120018104713
          TwoMutexPriorityMutex Low Priority: 120017001761, High Priority: 120021721189
MutexAndAtomicBoolPriorityMutex Low Priority: 120016876760, High Priority: 120020554118
   MutexAndTwoBoolPriorityMutex Low Priority: 120017668521, High Priority: 120021849214
   1000000,        100,         10
             BasicPriorityMutex Low Priority: 120015159858, High Priority: 120016359708
          TwoMutexPriorityMutex Low Priority: 120016648402, High Priority: 120019679740
MutexAndAtomicBoolPriorityMutex Low Priority: 120016356825, High Priority: 120018199368
   MutexAndTwoBoolPriorityMutex Low Priority: 120015952659, High Priority: 120018182257
   1000000,        100,        100
             BasicPriorityMutex Low Priority: 120015706500, High Priority: 120016749570
          TwoMutexPriorityMutex Low Priority: 120016351243, High Priority: 120007866016
MutexAndAtomicBoolPriorityMutex Low Priority: 120017179543, High Priority: 120008324306
   MutexAndTwoBoolPriorityMutex Low Priority: 120016235114, High Priority: 120009120069
   1000000,        100,       1000
             BasicPriorityMutex Low Priority: 120016707281, High Priority: 120016916807
          TwoMutexPriorityMutex Low Priority: 120015738892, High Priority: 119893266475
MutexAndAtomicBoolPriorityMutex Low Priority: 120017102387, High Priority: 119893439663
   MutexAndTwoBoolPriorityMutex Low Priority: 120016721445, High Priority: 119894628719
   1000000,        100,      10000
             BasicPriorityMutex Low Priority: 120017524542, High Priority: 120008816066
          TwoMutexPriorityMutex Low Priority: 120015830364, High Priority: 118812736835
MutexAndAtomicBoolPriorityMutex Low Priority: 120015995881, High Priority: 118814335702
   MutexAndTwoBoolPriorityMutex Low Priority: 120015913891, High Priority: 118812619815
   1000000,        100,     100000
             BasicPriorityMutex Low Priority: 120016799851, High Priority: 119817951565
          TwoMutexPriorityMutex Low Priority: 120015383258, High Priority: 108010066209
MutexAndAtomicBoolPriorityMutex Low Priority: 120016216430, High Priority: 108011281699
   MutexAndTwoBoolPriorityMutex Low Priority: 120017389161, High Priority: 108012303604
   1000000,        100,    1000000
             BasicPriorityMutex Low Priority: 120016707713, High Priority: 119017853362
          TwoMutexPriorityMutex Low Priority: 120016455654, High Priority:  20007820207
MutexAndAtomicBoolPriorityMutex Low Priority: 120017232559, High Priority:  14007501781
   MutexAndTwoBoolPriorityMutex Low Priority: 120016357889, High Priority:  18008583045
   1000000,       1000,          1
             BasicPriorityMutex Low Priority: 120014061278, High Priority: 120015227733
          TwoMutexPriorityMutex Low Priority: 120017450343, High Priority: 120026971622
MutexAndAtomicBoolPriorityMutex Low Priority: 120016179377, High Priority: 120021005559
   MutexAndTwoBoolPriorityMutex Low Priority: 120016570249, High Priority: 120024719858
   1000000,       1000,         10
             BasicPriorityMutex Low Priority: 120016726862, High Priority: 120017903720
          TwoMutexPriorityMutex Low Priority: 120016147761, High Priority: 120023990575
MutexAndAtomicBoolPriorityMutex Low Priority: 120016357644, High Priority: 120021848308
   MutexAndTwoBoolPriorityMutex Low Priority: 120016055735, High Priority: 120022584561
   1000000,       1000,        100
             BasicPriorityMutex Low Priority: 120015601163, High Priority: 120016804051
          TwoMutexPriorityMutex Low Priority: 120016402933, High Priority: 120013823468
MutexAndAtomicBoolPriorityMutex Low Priority: 120016383854, High Priority: 120011505603
   MutexAndTwoBoolPriorityMutex Low Priority: 120016876483, High Priority: 120013880747
   1000000,       1000,       1000
             BasicPriorityMutex Low Priority: 120015581782, High Priority: 120015613183
          TwoMutexPriorityMutex Low Priority: 120017190344, High Priority: 119900198911
MutexAndAtomicBoolPriorityMutex Low Priority: 120016796816, High Priority: 119897803998
   MutexAndTwoBoolPriorityMutex Low Priority: 120016336081, High Priority: 119898691249
   1000000,       1000,      10000
             BasicPriorityMutex Low Priority: 120015259608, High Priority: 120006498199
          TwoMutexPriorityMutex Low Priority: 120017558169, High Priority: 118819021947
MutexAndAtomicBoolPriorityMutex Low Priority: 120017897828, High Priority: 118817832260
   MutexAndTwoBoolPriorityMutex Low Priority: 120016425135, High Priority: 118817223181
   1000000,       1000,     100000
             BasicPriorityMutex Low Priority: 120015493861, High Priority: 119916604168
          TwoMutexPriorityMutex Low Priority: 120016735186, High Priority: 108017412362
MutexAndAtomicBoolPriorityMutex Low Priority: 120017194348, High Priority: 108016652579
   MutexAndTwoBoolPriorityMutex Low Priority: 120015304327, High Priority: 108015387656
   1000000,       1000,    1000000
             BasicPriorityMutex Low Priority: 120016745645, High Priority: 119017939013
          TwoMutexPriorityMutex Low Priority: 120015535056, High Priority:  12011517366
MutexAndAtomicBoolPriorityMutex Low Priority: 120015872805, High Priority:  18010249599
   MutexAndTwoBoolPriorityMutex Low Priority: 120016873629, High Priority:   8010819027
   1000000,      10000,          1
             BasicPriorityMutex Low Priority: 120015753787, High Priority: 120016962840
          TwoMutexPriorityMutex Low Priority: 119017254136, High Priority: 119026483985
MutexAndAtomicBoolPriorityMutex Low Priority: 119017687149, High Priority: 119024226187
   MutexAndTwoBoolPriorityMutex Low Priority: 119016177072, High Priority: 119025429873
   1000000,      10000,         10
             BasicPriorityMutex Low Priority: 120015846045, High Priority: 120017040205
          TwoMutexPriorityMutex Low Priority: 119016775151, High Priority: 119025173673
MutexAndAtomicBoolPriorityMutex Low Priority: 119015868301, High Priority: 119022015125
   MutexAndTwoBoolPriorityMutex Low Priority: 119016751206, High Priority: 119023806930
   1000000,      10000,        100
             BasicPriorityMutex Low Priority: 120016940850, High Priority: 120018085175
          TwoMutexPriorityMutex Low Priority: 119016714812, High Priority: 119014785409
MutexAndAtomicBoolPriorityMutex Low Priority: 119016138832, High Priority: 119012018953
   MutexAndTwoBoolPriorityMutex Low Priority: 119016342108, High Priority: 119014162534
   1000000,      10000,       1000
             BasicPriorityMutex Low Priority: 120016693701, High Priority: 120016861079
          TwoMutexPriorityMutex Low Priority: 119016690524, High Priority: 118898098967
MutexAndAtomicBoolPriorityMutex Low Priority: 119015952954, High Priority: 118897967429
   MutexAndTwoBoolPriorityMutex Low Priority: 119016429123, High Priority: 118900745243
   1000000,      10000,      10000
             BasicPriorityMutex Low Priority: 120017301070, High Priority: 120008445663
          TwoMutexPriorityMutex Low Priority: 119015961049, High Priority: 117829655061
MutexAndAtomicBoolPriorityMutex Low Priority: 119016903102, High Priority: 117828350366
   MutexAndTwoBoolPriorityMutex Low Priority: 119017148310, High Priority: 117828908945
   1000000,      10000,     100000
             BasicPriorityMutex Low Priority: 120015505139, High Priority: 119916791829
          TwoMutexPriorityMutex Low Priority: 119016231996, High Priority: 107117411297
MutexAndAtomicBoolPriorityMutex Low Priority: 119016844469, High Priority: 107117488057
   MutexAndTwoBoolPriorityMutex Low Priority: 119016984651, High Priority: 107118527747
   1000000,      10000,    1000000
             BasicPriorityMutex Low Priority: 120016648565, High Priority: 114017141212
          TwoMutexPriorityMutex Low Priority: 119016359563, High Priority:  10012708691
MutexAndAtomicBoolPriorityMutex Low Priority: 119015967716, High Priority:  13011801815
   MutexAndTwoBoolPriorityMutex Low Priority: 119016047539, High Priority:  15011662634
   1000000,     100000,          1
             BasicPriorityMutex Low Priority: 120018348499, High Priority: 120019693509
          TwoMutexPriorityMutex Low Priority: 110015597609, High Priority: 110024645937
MutexAndAtomicBoolPriorityMutex Low Priority: 100014115833, High Priority: 100019747753
   MutexAndTwoBoolPriorityMutex Low Priority:  97014006173, High Priority:  97019592261
   1000000,     100000,         10
             BasicPriorityMutex Low Priority: 120016231237, High Priority: 120017421351
          TwoMutexPriorityMutex Low Priority: 110015571199, High Priority: 110024149876
MutexAndAtomicBoolPriorityMutex Low Priority: 109015180395, High Priority: 109021460619
   MutexAndTwoBoolPriorityMutex Low Priority: 109015334270, High Priority: 109022679681
   1000000,     100000,        100
             BasicPriorityMutex Low Priority: 120016153417, High Priority: 120017310329
          TwoMutexPriorityMutex Low Priority: 110015254817, High Priority: 110012046551
MutexAndAtomicBoolPriorityMutex Low Priority: 110014584934, High Priority: 110010877599
   MutexAndTwoBoolPriorityMutex Low Priority: 110014907292, High Priority: 110012608908
   1000000,     100000,       1000
             BasicPriorityMutex Low Priority: 120015249692, High Priority: 120015552343
          TwoMutexPriorityMutex Low Priority: 110015694773, High Priority: 109909613435
MutexAndAtomicBoolPriorityMutex Low Priority: 110015727925, High Priority: 109907185447
   MutexAndTwoBoolPriorityMutex Low Priority: 110015697909, High Priority: 109908325798
   1000000,     100000,      10000
             BasicPriorityMutex Low Priority: 120016398391, High Priority: 120007588396
          TwoMutexPriorityMutex Low Priority: 110015871622, High Priority: 108919383244
MutexAndAtomicBoolPriorityMutex Low Priority: 110015314830, High Priority: 108916134382
   MutexAndTwoBoolPriorityMutex Low Priority: 110015283445, High Priority: 108915234753
   1000000,     100000,     100000
             BasicPriorityMutex Low Priority: 120017027366, High Priority: 119918125377
          TwoMutexPriorityMutex Low Priority: 110014582015, High Priority:  99016748836
MutexAndAtomicBoolPriorityMutex Low Priority: 110015439897, High Priority:  99016616057
   MutexAndTwoBoolPriorityMutex Low Priority: 110014799460, High Priority:  99014593816
   1000000,     100000,    1000000
             BasicPriorityMutex Low Priority: 120017209739, High Priority: 119018379393
          TwoMutexPriorityMutex Low Priority: 110015398827, High Priority:   4012183501
MutexAndAtomicBoolPriorityMutex Low Priority: 111014862823, High Priority:  14010396406
   MutexAndTwoBoolPriorityMutex Low Priority: 111014711660, High Priority:  12011439881
   1000000,    1000000,          1
             BasicPriorityMutex Low Priority: 120016864888, High Priority: 120018216250
          TwoMutexPriorityMutex Low Priority:  61008378366, High Priority:  60013291633
MutexAndAtomicBoolPriorityMutex Low Priority:  42005898860, High Priority:  41007648848
   MutexAndTwoBoolPriorityMutex Low Priority:  43005559407, High Priority:  42007961607
   1000000,    1000000,         10
             BasicPriorityMutex Low Priority: 120015924362, High Priority: 120017076224
          TwoMutexPriorityMutex Low Priority:  61008447516, High Priority:  60011610420
MutexAndAtomicBoolPriorityMutex Low Priority:  59008732309, High Priority:  58012106695
   MutexAndTwoBoolPriorityMutex Low Priority:  56007325035, High Priority:  55010775242
   1000000,    1000000,        100
             BasicPriorityMutex Low Priority: 120017078033, High Priority: 120018353790
          TwoMutexPriorityMutex Low Priority:  61008060119, High Priority:  60007182374
MutexAndAtomicBoolPriorityMutex Low Priority:  61008155914, High Priority:  60006079861
   MutexAndTwoBoolPriorityMutex Low Priority:  61008121805, High Priority:  60006858732
   1000000,    1000000,       1000
             BasicPriorityMutex Low Priority: 120017185919, High Priority: 120017486479
          TwoMutexPriorityMutex Low Priority:  61008636174, High Priority:  59950721478
MutexAndAtomicBoolPriorityMutex Low Priority:  61007928578, High Priority:  59948579497
   MutexAndTwoBoolPriorityMutex Low Priority:  61008274847, High Priority:  59950077946
   1000000,    1000000,      10000
             BasicPriorityMutex Low Priority: 120016497783, High Priority: 120007703941
          TwoMutexPriorityMutex Low Priority:  61007534025, High Priority:  59408079535
MutexAndAtomicBoolPriorityMutex Low Priority:  61008627700, High Priority:  59408923235
   MutexAndTwoBoolPriorityMutex Low Priority:  61008317693, High Priority:  59409126692
   1000000,    1000000,     100000
             BasicPriorityMutex Low Priority: 120016588541, High Priority: 119917813814
          TwoMutexPriorityMutex Low Priority:  61007833562, High Priority:  54008344134
MutexAndAtomicBoolPriorityMutex Low Priority:  61008833692, High Priority:  54009896728
   MutexAndTwoBoolPriorityMutex Low Priority:  61008339612, High Priority:  54008569284
   1000000,    1000000,    1000000
             BasicPriorityMutex Low Priority: 120015459526, High Priority: 119016638972
          TwoMutexPriorityMutex Low Priority:  65008533734, High Priority:   9007203629
MutexAndAtomicBoolPriorityMutex Low Priority:  64007872544, High Priority:   6005941981
   MutexAndTwoBoolPriorityMutex Low Priority:  64008175722, High Priority:   6006895968
Algorithm win counts for Low Priority Thread amount of work:
  BasicPriorityMutex: 272
  MutexAndAtomicBoolPriorityMutex: 10
  MutexAndTwoBoolPriorityMutex: 8
  TwoMutexPriorityMutex: 53
Algorithm win counts for High Priority Thread lowest latency:
  BasicPriorityMutex: 7
  MutexAndAtomicBoolPriorityMutex: 268
  MutexAndTwoBoolPriorityMutex: 40
  TwoMutexPriorityMutex: 28
```