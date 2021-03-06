//=======================================================================
// Regional Earth System Model (RegESM)
// Copyright (c) 2013-2019 Ufuk Turuncoglu
// Licensed under the MIT License.
//=======================================================================
#include "mpi.h"
#include "grid.h"
#include "vtkMPI.h"
#include "vtkDataSet.h"
#include "vtkCPProcessor.h"
#include "vtkCPDataDescription.h"
#include "vtkCPPythonScriptPipeline.h"
#include "vtkCPInputDataDescription.h"
#include "vtkPointData.h"
#include "vtkCellData.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkCompositeDataIterator.h"

#include <vector>

// This code is meant as an API for Fortran and C simulation codes.
namespace ParaViewCoProcessing {

  /// Clear all of the field data from the grids.
  void ClearFieldDataFromGrid(vtkDataSet* grid) {
    if (grid) {
      grid->GetPointData()->Initialize();
      grid->GetCellData()->Initialize();
      // probably don't need to clear out field data but just being safe
      grid->GetFieldData()->Initialize();
    }
  }
}

namespace {
  //
  // catalyst coprocessor
  //
  vtkCPProcessor* g_coprocessor;

  //
  // coprocessor data
  //
  vtkCPDataDescription* g_coprocessorData;

  //
  // is time data set?
  //
  bool g_isTimeDataSet;

  //
  // rectilinear grid
  //
  std::vector<ESMFAdaptor::Grid<ESMFAdaptor::RECTILINEAR> > g_grid;
  std::vector<ESMFAdaptor::Grid<ESMFAdaptor::RECTILINEAR> >::iterator g_iter;
};

//////////////////////////////////////////////////////////////////////

extern "C" void my_coprocessorinitializewithpython_(int *fcomm, const char pythonScriptNames[][255], int *nscript, const char strarr[][255], int *size) {
  if (pythonScriptNames != NULL) {
    if (!g_coprocessor) {
      g_coprocessor = vtkCPProcessor::New();
      MPI_Comm handle = MPI_Comm_f2c(*fcomm);
      vtkMPICommunicatorOpaqueComm *Comm = new vtkMPICommunicatorOpaqueComm(&handle);
      g_coprocessor->Initialize(*Comm);
      for (int i = 0; i < *nscript; i++) {
        vtkSmartPointer<vtkCPPythonScriptPipeline> pipeline = vtkSmartPointer<vtkCPPythonScriptPipeline>::New();
        pipeline->Initialize(pythonScriptNames[i]);
        g_coprocessor->AddPipeline(pipeline);
      }
    }

    if (!g_coprocessorData) {
      g_coprocessorData = vtkCPDataDescription::New();

      // must be input port for all model components and for all dimensions
      for (int i = 0; i < *size; i++) {
        g_coprocessorData->AddInput(strarr[i]);
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////

extern "C" void my_requestdatadescription_(int* timeStep, double* time, int* coprocessThisTimeStep) {
  if (!g_coprocessorData || !g_coprocessor) {
    vtkGenericWarningMacro("Problem in needtocoprocessthistimestep."
                           << "Probably need to initialize.");
    *coprocessThisTimeStep = 0;
    return;
  }

  vtkIdType tStep = *timeStep;
  g_coprocessorData->SetTimeData(*time, tStep);

  if (g_coprocessor->RequestDataDescription(g_coprocessorData)) {
    g_isTimeDataSet = true;
    *coprocessThisTimeStep = 1;
  } else {
    g_isTimeDataSet = false;
    *coprocessThisTimeStep = 0;
  }
}

////////////////////////////////////////////////////////////////////n

extern "C" void my_needtocreategrid_(int* needGrid, const char* name) {
  // check time
  if(!g_isTimeDataSet) {
    vtkGenericWarningMacro("Time data not set.");
    *needGrid = 0;
    return;
  }
  // check grids
  if (g_coprocessorData->GetInputDescriptionByName(name)->GetGrid()) {
    *needGrid = 0;
    if (vtkDataSet* grid = vtkDataSet::SafeDownCast(g_coprocessorData->GetInputDescriptionByName(name)->GetGrid())) {
      ParaViewCoProcessing::ClearFieldDataFromGrid(grid);
    } else {
      vtkMultiBlockDataSet* multiBlock = vtkMultiBlockDataSet::SafeDownCast(g_coprocessorData->GetInputDescriptionByName(name)->GetGrid());
      if (multiBlock) {
        vtkCompositeDataIterator* iter = multiBlock->NewIterator();
        iter->InitTraversal();
        for (iter->GoToFirstItem(); !iter->IsDoneWithTraversal(); iter->GoToNextItem()) {
          ParaViewCoProcessing::ClearFieldDataFromGrid(vtkDataSet::SafeDownCast(iter->GetCurrentDataObject()));
        }
        iter->Delete();
      }
    }
  } else {
    *needGrid = 1;
  }
}

////////////////////////////////////////////////////////////////////n

extern "C" void my_coprocess_() {
  if(!g_isTimeDataSet) {
    vtkGenericWarningMacro("Time data not set.");
  } else {
    g_coprocessor->CoProcess(g_coprocessorData);
  }
  // Reset time data.
  g_isTimeDataSet = false;
}

////////////////////////////////////////////////////////////////////n

extern "C" void create_grid(const char* name, int nProc, int myRank, int* dims, int* lb, int* ub, int nPoints, float* lonCoord, float* latCoord, float* levCoord) {
  if (!g_coprocessorData) {
    vtkGenericWarningMacro("Unable to access CoProcessorData.");
    return;
  }

  //
  // create temporary grid object
  //
  ESMFAdaptor::Grid<ESMFAdaptor::RECTILINEAR>* grid = new ESMFAdaptor::Grid<ESMFAdaptor::RECTILINEAR>();

  //
  // set attributes
  //
  grid->SetName(name);
  grid->SetNProc(nProc);
  grid->SetMPIRank(myRank);
  grid->SetNPoints(nPoints);
  grid->SetLon(dims[0], nPoints, lonCoord);
  grid->SetLat(dims[1], nPoints, latCoord);
  if (levCoord != 0) {
    grid->SetLev(dims[2], nPoints, levCoord);
  } 
  grid->SetBounds(lb, ub);

  // create grid
  grid->Create(name);

  // create input port for grid
  if (levCoord == 0) {
    if (! ESMFAdaptor::Grid<ESMFAdaptor::RECTILINEAR>::SetToCoprocessor(g_coprocessorData, name, dims, grid->GetGrid())) {
      vtkGenericWarningMacro(<< "No input data description for '" << name << "'");
      delete grid;
      grid = NULL; 
    }
  } else {
    if (! ESMFAdaptor::Grid<ESMFAdaptor::RECTILINEAR>::SetToCoprocessor(g_coprocessorData, name, dims, grid->GetGrid())) {
      vtkGenericWarningMacro(<< "No input data description for '" << name << "'");
      delete grid;
      grid = NULL; 
    }
  } 

  //
  // add grid to vector
  //
  g_grid.push_back(*grid); 
}

//
// Add field(s) to the data container
//
extern "C" void add_scalar_(float* data, char* name, int* size, int* mpiSize, int* mpiRank, const char* iname) {
  for (g_iter = g_grid.begin(); g_iter != g_grid.end(); g_iter++) {
    char *dumm = g_iter->GetName();
    if (strcmp(dumm, iname) >= 0) {
      g_iter->SetAttributeValue(g_coprocessorData, data, name, iname, size, mpiSize, mpiRank);
    }
  } 
}
