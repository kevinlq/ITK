/*=========================================================================
 *
 *  Copyright Insight Software Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
/*=========================================================================
 *
 *  Portions of this file are subject to the VTK Toolkit Version 3 copyright.
 *
 *  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
 *
 *  For complete copyright, license and disclaimer of warranty information
 *  please refer to the NOTICE file at the top of the ITK source tree.
 *
 *=========================================================================*/
#include "itkPoolMultiThreader.h"
#include "itkNumericTraits.h"
#include "itkProcessObject.h"
#include "itkImageSourceCommon.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <algorithm>

namespace itk
{

PoolMultiThreader::PoolMultiThreader()
  : m_ThreadPool(ThreadPool::GetInstance())
{
  for (ThreadIdType i = 0; i < ITK_MAX_THREADS; ++i)
  {
    m_ThreadInfoArray[i].WorkUnitID = i;
  }

  ThreadIdType defaultThreads = std::max(1u, GetGlobalDefaultNumberOfThreads());
#if !defined(ITKV4_COMPATIBILITY)
  if (defaultThreads > 1) // one work unit for only one thread
  {
    defaultThreads *= 4;
  }
#endif
  m_NumberOfWorkUnits = std::min<ThreadIdType>(ITK_MAX_THREADS, defaultThreads);
  m_MaximumNumberOfThreads = m_ThreadPool->GetMaximumNumberOfThreads();
}

PoolMultiThreader::~PoolMultiThreader() = default;

void
PoolMultiThreader::SetSingleMethod(ThreadFunctionType f, void * data)
{
  m_SingleMethod = f;
  m_SingleData = data;
}

void
PoolMultiThreader::SetMaximumNumberOfThreads(ThreadIdType numberOfThreads)
{
  Superclass::SetMaximumNumberOfThreads(numberOfThreads);
  ThreadIdType threadCount = m_ThreadPool->GetMaximumNumberOfThreads();
  if (threadCount < m_MaximumNumberOfThreads)
  {
    m_ThreadPool->AddThreads(m_MaximumNumberOfThreads - threadCount);
  }
  m_MaximumNumberOfThreads = m_ThreadPool->GetMaximumNumberOfThreads();
}

#define ExceptionHandlingPrologue                                                                                      \
  bool abortOccurred = false;                                                                                          \
  bool exceptionOccurred = false;                                                                                      \
                                                                                                                       \
  ProcessAborted abortException;                                                                                       \
  std::string    exceptionDetails;

#define WrapExceptionHandling(statement)                                                                               \
  try                                                                                                                  \
  {                                                                                                                    \
    statement;                                                                                                         \
  }                                                                                                                    \
  catch (ProcessAborted & e)                                                                                           \
  {                                                                                                                    \
    abortOccurred = true;                                                                                              \
    abortException = e;                                                                                                \
  }                                                                                                                    \
  catch (std::exception & e)                                                                                           \
  {                                                                                                                    \
    exceptionDetails = e.what();                                                                                       \
    exceptionOccurred = true;                                                                                          \
  }                                                                                                                    \
  catch (...)                                                                                                          \
  {                                                                                                                    \
    exceptionOccurred = true;                                                                                          \
  }

#define ExceptionHandlingEpilogue                                                                                      \
  if (abortOccurred)                                                                                                   \
  {                                                                                                                    \
    throw abortException;                                                                                              \
  }                                                                                                                    \
  if (exceptionOccurred)                                                                                               \
  {                                                                                                                    \
    if (exceptionDetails.empty())                                                                                      \
    {                                                                                                                  \
      itkExceptionMacro("Exception occurred during SingleMethodExecute");                                              \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
      itkExceptionMacro(<< "Exception occurred during SingleMethodExecute" << std::endl << exceptionDetails);          \
    }                                                                                                                  \
  }

void
PoolMultiThreader::SingleMethodExecute()
{
  ThreadIdType threadLoop = 0;

  if (!m_SingleMethod)
  {
    itkExceptionMacro(<< "No single method set!");
  }

  // obey the global maximum number of threads limit
  m_NumberOfWorkUnits = std::min(this->GetGlobalMaximumNumberOfThreads(), m_NumberOfWorkUnits);

  ExceptionHandlingPrologue;

  for (threadLoop = 1; threadLoop < m_NumberOfWorkUnits; ++threadLoop)
  {
    m_ThreadInfoArray[threadLoop].UserData = m_SingleData;
    m_ThreadInfoArray[threadLoop].NumberOfWorkUnits = m_NumberOfWorkUnits;
    m_ThreadInfoArray[threadLoop].Future = m_ThreadPool->AddWork(m_SingleMethod, &m_ThreadInfoArray[threadLoop]);
  }

  // Now, the parent thread calls this->SingleMethod() itself
  m_ThreadInfoArray[0].UserData = m_SingleData;
  m_ThreadInfoArray[0].NumberOfWorkUnits = m_NumberOfWorkUnits;
  WrapExceptionHandling(m_SingleMethod((void *)(&m_ThreadInfoArray[0])));

  // The parent thread has finished SingleMethod()
  // so now it waits for each of the other work units to finish
  for (threadLoop = 1; threadLoop < m_NumberOfWorkUnits; ++threadLoop)
  {
    WrapExceptionHandling(m_ThreadInfoArray[threadLoop].Future.get());
  }

  ExceptionHandlingEpilogue;
}

void
PoolMultiThreader ::ParallelizeArray(SizeValueType             firstIndex,
                                     SizeValueType             lastIndexPlus1,
                                     ArrayThreadingFunctorType aFunc,
                                     ProcessObject *           filter)
{
  MultiThreaderBase::HandleFilterProgress(filter, 0.0f);

  if (firstIndex + 1 < lastIndexPlus1)
  {
    SizeValueType chunkSize = (lastIndexPlus1 - firstIndex) / m_NumberOfWorkUnits;
    if ((lastIndexPlus1 - firstIndex) % m_NumberOfWorkUnits > 0)
    {
      chunkSize++; // we want slightly bigger chunks to be processed first
    }

    auto lambda = [aFunc](SizeValueType start, SizeValueType end) {
      for (SizeValueType ii = start; ii < end; ii++)
      {
        aFunc(ii);
      }
      // make this lambda have the same signature as m_SingleMethod
      return ITK_THREAD_RETURN_DEFAULT_VALUE;
    };

    SizeValueType workUnit = 1;
    for (SizeValueType i = firstIndex + chunkSize; i < lastIndexPlus1; i += chunkSize)
    {
      m_ThreadInfoArray[workUnit++].Future = m_ThreadPool->AddWork(lambda, i, std::min(i + chunkSize, lastIndexPlus1));
    }
    itkAssertOrThrowMacro(workUnit <= m_NumberOfWorkUnits, "Number of work units was somehow miscounted!");

    ExceptionHandlingPrologue;

    // execute this thread's share
    WrapExceptionHandling(lambda(firstIndex, firstIndex + chunkSize));

    // now wait for the other computations to finish
    for (SizeValueType i = 1; i < workUnit; i++)
    {
      if (filter)
      {
        filter->UpdateProgress(i / float(workUnit));
      }

      WrapExceptionHandling(m_ThreadInfoArray[i].Future.get());
    }

    ExceptionHandlingEpilogue;
  }
  else if (firstIndex + 1 == lastIndexPlus1)
  {
    aFunc(firstIndex);
  }
  // else nothing needs to be executed

  MultiThreaderBase::HandleFilterProgress(filter, 1.0f);
}

void
PoolMultiThreader ::ParallelizeImageRegion(unsigned int         dimension,
                                           const IndexValueType index[],
                                           const SizeValueType  size[],
                                           ThreadingFunctorType funcP,
                                           ProcessObject *      filter)
{
  MultiThreaderBase::HandleFilterProgress(filter, 0.0f);

  if (m_NumberOfWorkUnits == 1) // no multi-threading wanted
  {
    funcP(index, size); // process whole region
  }
  else
  {
    ImageIORegion region(dimension);
    for (unsigned d = 0; d < dimension; d++)
    {
      region.SetIndex(d, index[d]);
      region.SetSize(d, size[d]);
    }
    if (region.GetNumberOfPixels() <= 1)
    {
      funcP(index, size); // process whole region
    }
    else
    {
      const ImageRegionSplitterBase * splitter = ImageSourceCommon::GetGlobalDefaultSplitter();
      ThreadIdType                    splitCount = splitter->GetNumberOfSplits(region, m_NumberOfWorkUnits);
      itkAssertOrThrowMacro(splitCount <= m_NumberOfWorkUnits, "Split count is greater than number of work units!");
      ImageIORegion iRegion;
      ThreadIdType  total;
      for (ThreadIdType i = 1; i < splitCount; i++)
      {
        iRegion = region;
        total = splitter->GetSplit(i, splitCount, iRegion);
        if (i < total)
        {
          m_ThreadInfoArray[i].Future = m_ThreadPool->AddWork([funcP, iRegion]() {
            funcP(&iRegion.GetIndex()[0], &iRegion.GetSize()[0]);
            // make this lambda have the same signature as m_SingleMethod
            return ITK_THREAD_RETURN_DEFAULT_VALUE;
          });
        }
        else
        {
          itkExceptionMacro("Could not get work unit "
                            << i << " even though we checked possible number of splits beforehand!");
        }
      }
      iRegion = region;
      total = splitter->GetSplit(0, splitCount, iRegion);

      ExceptionHandlingPrologue;

      // execute this thread's share
      WrapExceptionHandling(funcP(&iRegion.GetIndex()[0], &iRegion.GetSize()[0]));

      // now wait for the other computations to finish
      for (ThreadIdType i = 1; i < splitCount; i++)
      {
        if (filter)
        {
          filter->UpdateProgress(i / float(splitCount));
        }
        WrapExceptionHandling(m_ThreadInfoArray[i].Future.get());
      }

      ExceptionHandlingEpilogue;
    }
  }
  MultiThreaderBase::HandleFilterProgress(filter, 1.0f);
}

void
PoolMultiThreader::PrintSelf(std::ostream & os, Indent indent) const
{
  Superclass::PrintSelf(os, indent);
}

} // namespace itk
