target_sources(swiftSILOptimizer PRIVATE
  ARCAnalysis.cpp
  AccessSummaryAnalysis.cpp
  AccessStorageAnalysis.cpp
  AliasAnalysis.cpp
  Analysis.cpp
  ArraySemantic.cpp
  BasicCalleeAnalysis.cpp
  CallerAnalysis.cpp
  ClassHierarchyAnalysis.cpp
  ClosureScope.cpp
  ColdBlockInfo.cpp
  DeadEndBlocksAnalysis.cpp
  DestructorAnalysis.cpp
  DifferentiableActivityAnalysis.cpp
  EpilogueARCAnalysis.cpp
  FunctionOrder.cpp
  IVAnalysis.cpp
  LoopAnalysis.cpp
  LoopRegionAnalysis.cpp
  MemoryBehavior.cpp
  NonLocalAccessBlockAnalysis.cpp
  PassManagerVerifierAnalysis.cpp
  ProtocolConformanceAnalysis.cpp
  RCIdentityAnalysis.cpp
  SimplifyInstruction.cpp
  TypeExpansionAnalysis.cpp
  ValueTracking.cpp)
