/*v1*************************************************************************************/
/* This is developed at Carnegie Mellon University in collaboration with Autel Robotics */
/*                                                                                      */
/* PI:                                                                                  */
/* George Kantor                                                                        */
/*                                                                                      */
/* Authors:                                                                             */ 
/* Weizhao Shao                                                                         */
/* Cong Li                                                                              */
/* Srinivasan Vijayarangan                                                              */
/*                                                                                      */
/* Please refer to the contract document for details on license/copyright information.  */
/****************************************************************************************/
/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    BatchFixedLagSmoother.cpp
 * @brief   An LM-based fixed-lag smoother.
 *
 * @author  Michael Kaess, Stephen Williams
 * @date    Oct 14, 2012
 */

#include <vio/BatchFixedLagSmoother.h>
#include <gtsam/nonlinear/LinearContainerFactor.h>
#include <gtsam/linear/GaussianJunctionTree.h>
#include <gtsam/linear/GaussianFactorGraph.h>
#include <gtsam/linear/GaussianFactor.h>

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <Eigen/StdVector>

using namespace std;
using namespace gtsam;

// For eigen output
//Eigen::IOFormat OctaveFmt(Eigen::StreamPrecision, 0, ", ", ";\n", "", "", "[", "]");
//std::string sep = "\n----------------------------------------\n";


/* ************************************************************************* */
void BatchFixedLagSmoother::print(const string& s,
    const KeyFormatter& keyFormatter) const {
  vioFixedLagSmoother::print(s, keyFormatter);
  // TODO: What else to print?
}

/* ************************************************************************* */
bool BatchFixedLagSmoother::equals(const vioFixedLagSmoother& rhs,
    double tol) const {
  const BatchFixedLagSmoother* e =
      dynamic_cast<const BatchFixedLagSmoother*>(&rhs);
  return e != NULL && vioFixedLagSmoother::equals(*e, tol)
      && factors_.equals(e->factors_, tol) && theta_.equals(e->theta_, tol);
}

/* ************************************************************************* */
Matrix BatchFixedLagSmoother::marginalCovariance(Key key) const {
  throw runtime_error(
      "BatchFixedLagSmoother::marginalCovariance not implemented");
}

/* ************************************************************************* */
vioFixedLagSmoother::Result BatchFixedLagSmoother::update(
    const NonlinearFactorGraph& newFactors, const Values& newTheta,
    const KeyTimestampMap& timestamps) {


  clock_t start(clock());
  // Update all of the internal variables with the new information
  gttic(augment_system);
  // Add the new variables to theta
  theta_.insert(newTheta);
  // Add new variables to the end of the ordering
  for (const auto& key_value : newTheta) {
    ordering_.push_back(key_value.key);
  }
  // Augment Delta
  delta_.insert(newTheta.zeroVectors());
  //PrintSymbolicGraph(newFactors, "New Factors to graph in Batch:\n");
  // calculated time
  //float diff( -((float)start-(float)clock())/CLOCKS_PER_SEC );
  //std::cout << "insertion time: " << std::setprecision(5) << diff*1.0e3 << "ms"<< std::endl;

  // Add the new factors to the graph, updating the variable index
  insertFactors(newFactors);
  gttoc(augment_system);

  // Update the Timestamps associated with the factor keys
  updateKeyTimestampMap(timestamps);
  /*
  std::cout << "key timestamp map: \n";
  for(auto it = keyTimestampMap_.begin(); it != keyTimestampMap_.end(); ++it)
    std::cout << DefaultKeyFormatter(it->first) << " " << it->second << "\n";

  std::cout << "timestamp key map: \n";
  for(auto it = timestampKeyMap_.begin(); it != timestampKeyMap_.end(); ++it)
    std::cout << it->first << " " << DefaultKeyFormatter(it->second) << "\n";
  */
  // Get current timestamp
  double current_timestamp = getCurrentTimestamp();

  // Find the set of variables to be marginalized out
  KeyVector marginalizableKeys = findKeysBefore(
      current_timestamp - smootherLag_);
  /*
  std::cout << "current_timstamp: " << current_timestamp 
            << " smoothing lag " << smootherLag_ << std::endl;  
  std::cout << "key to marginalize with size: " << marginalizableKeys.size() << std::endl;
  for (Key key: marginalizableKeys)
    std::cout << DefaultKeyFormatter(key) << " \n";
  */
  // Reorder
  gttic(reorder);
  reorder(marginalizableKeys);
  gttoc(reorder);

  //PrintSymbolicGraph(factors_, "Graph in Batch:\n");
  //factors_.print();
  // Optimize
  //std::cout << "entering the optimization.\n";
  gttic(optimize);
  Result result;
  if (factors_.size() > 0) {
    result = optimize();
  }
  gttoc(optimize);

  // calculated time
  //float diff = ( -((float)start-(float)clock())/CLOCKS_PER_SEC );
  //std::cout << "optimize time: " << std::setprecision(5) << diff*1.0e3 << "ms"<< std::endl;
  
  // Marginalize out old variables.
  gttic(marginalize);
  if (marginalizableKeys.size() > 0) {
    marginalize(marginalizableKeys);
  }
  gttoc(marginalize);

  return result;
}

/* ************************************************************************* */
void BatchFixedLagSmoother::insertFactors(
    const NonlinearFactorGraph& newFactors) {
  for(const auto& factor: newFactors) {
    Key index;
    // Insert the factor into an existing hole in the factor graph, if possible
    if (availableSlots_.size() > 0) {
      index = availableSlots_.front();
      availableSlots_.pop();
      factors_.replace(index, factor);
    } else {
      index = factors_.size();
      factors_.push_back(factor);
    }
    // Update the FactorIndex
    for(Key key: *factor) {
      factorIndex_[key].insert(index);
    }
  }
}

/* ************************************************************************* */
void BatchFixedLagSmoother::removeFactors(
    const set<size_t>& deleteFactors) {
  for(size_t slot: deleteFactors) {
    if (factors_.at(slot)) {
      // Remove references to this factor from the FactorIndex
      for(Key key: *(factors_.at(slot))) {
        factorIndex_[key].erase(slot);
      }
      // Remove the factor from the factor graph
      factors_.remove(slot);
      // Add the factor's old slot to the list of available slots
      availableSlots_.push(slot);
    } else {
      // TODO: Throw an error??
      cout << "Attempting to remove a factor from slot " << slot
          << ", but it is already NULL." << endl;
    }
  }
}

/* ************************************************************************* */
void BatchFixedLagSmoother::eraseKeys(const KeyVector& keys) {

  for(Key key: keys) {
    // Erase the key from the values
    theta_.erase(key);

    // Erase the key from the factor index
    factorIndex_.erase(key);

    // Erase the key from the set of linearized keys
    if (linearKeys_.exists(key)) {
      linearKeys_.erase(key);
    }
  }

  eraseKeyTimestampMap(keys);

  // Remove marginalized keys from the ordering and delta
  for(Key key: keys) {
    ordering_.erase(find(ordering_.begin(), ordering_.end(), key));
    delta_.erase(key);
  }
}

/* ************************************************************************* */
void BatchFixedLagSmoother::reorder(const KeyVector& marginalizeKeys) {
  // COLAMD groups will be used to place marginalize keys in Group 0, and everything else in Group 1
  ordering_ = Ordering::ColamdConstrainedFirst(factors_, marginalizeKeys);
}

/* ************************************************************************* */
vioFixedLagSmoother::Result BatchFixedLagSmoother::optimize() {

  // Create output result structure
  Result result;
  result.nonlinearVariables = theta_.size() - linearKeys_.size();
  result.linearVariables = linearKeys_.size();

  // Set optimization parameters
  double lambda = parameters_.lambdaInitial;
  double lambdaFactor = parameters_.lambdaFactor;
  double lambdaUpperBound = parameters_.lambdaUpperBound;
  double lambdaLowerBound = 1.0e-10;
  size_t maxIterations = parameters_.maxIterations;
  double relativeErrorTol = parameters_.relativeErrorTol;
  double absoluteErrorTol = parameters_.absoluteErrorTol;
  double errorTol = parameters_.errorTol;

  // Create a Values that holds the current evaluation point
  Values evalpoint = theta_.retract(delta_);
  result.error = factors_.error(evalpoint);

  // check if we're already close enough
  if (result.error <= errorTol) {
    return result;
  }

  // Use a custom optimization loop so the linearization points can be controlled
  double previousError;
  VectorValues newDelta;
  do {
    previousError = result.error;
    clock_t start(clock());

    // Do next iteration
    gttic(optimizer_iteration);
    {
      // Linearize graph around the linearization point
      GaussianFactorGraph linearFactorGraph = *factors_.linearize(theta_);

      // Keep increasing lambda until we make make progress
      while (true) {

        // Add prior factors at the current solution
        gttic(damp);
        GaussianFactorGraph dampedFactorGraph(linearFactorGraph);
        dampedFactorGraph.reserve(linearFactorGraph.size() + delta_.size());
        {
          // for each of the variables, add a prior at the current solution
          double sigma = 1.0 / sqrt(lambda);
          for(const auto& key_value: delta_) {
            size_t dim = key_value.second.size();
            Matrix A = Matrix::Identity(dim, dim);
            Vector b = key_value.second;
            SharedDiagonal model = noiseModel::Isotropic::Sigma(dim, sigma);
            GaussianFactor::shared_ptr prior(
                new JacobianFactor(key_value.first, A, b, model));
            dampedFactorGraph.push_back(prior);
          }
        }
        gttoc(damp);
        //std::cout << "b5 in optimize()\n";
        result.intermediateSteps++;

        gttic(solve);
        /*
        // check degeneracy, ill-posed linear system
        for (auto it=dampedFactorGraph.begin();it!=dampedFactorGraph.end();it++) {
          if ( (*it)==NULL )
            continue;
          Matrix infoMat = (*it)->information();
          //svd
          // Check size of infoMat
          size_t n = infoMat.rows(), p = infoMat.cols(), m = min(n,p);
          // Do SVD on infoMat
          Eigen::JacobiSVD<Matrix> svd(infoMat, Eigen::ComputeFullV);
          Vector s = svd.singularValues();
          // Find rank
          size_t rank = 0;
          for (size_t j = 0; j < m; j++) {
            //std::cout << "asda " << s(j) << "\n";
            if (s(j) > (1e-9)) rank++;
          }
          // output
          std::cout << "full rank is: "<< m <<" info matrix rank is: "<< rank << "\n"; 
        }
        //*/

        // Solve Damped Gaussian Factor Graph
        newDelta = dampedFactorGraph.optimize(ordering_,
            parameters_.getEliminationFunction());
        // update the evalpoint with the new delta
        //std::cout << "b5.5 in optimize()\n";
        evalpoint = theta_.retract(newDelta);
        gttoc(solve);

        //std::cout << "b6 in optimize()\n";
        // Evaluate the new error
        gttic(compute_error);
        double error = factors_.error(evalpoint);
        gttoc(compute_error);

        //std::cout << "b7 in optimize()\n";
        if (error < result.error) {
          // Keep this change
          // Update the error value
          result.error = error;
          // Update the linearization point
          theta_ = evalpoint;
          // Reset the deltas to zeros
          delta_.setZero();
          // Put the linearization points and deltas back for specific variables
          if (enforceConsistency_ && (linearKeys_.size() > 0)) {
            theta_.update(linearKeys_);
            for(const auto& key_value: linearKeys_) {
              delta_.at(key_value.key) = newDelta.at(key_value.key);
            }
          }
          // Decrease lambda for next time
          lambda /= lambdaFactor;
          if (lambda < lambdaLowerBound) {
            lambda = lambdaLowerBound;
          }
          // End this lambda search iteration
          break;
        } else {
          // Reject this change
          ///*
          if (APPLY_BOUND) {
            if (result.iterations == 0 && result.intermediateSteps > 4) {
              //cout << "iteration 0 intermediate steps reaches 5" << endl;
              break;
            }
            if (result.iterations > 0 && result.intermediateSteps > 1) {
              //cout << "iteration １ intermediate steps reaches 2" << endl;
              break;
            }
          }
          //*/
          if (lambda >= lambdaUpperBound) {
            // The maximum lambda has been used. Print a warning and end the search.
            cout
                << "Warning:  Levenberg-Marquardt giving up because cannot decrease error with maximum lambda"
                << endl;
            break;
          } else {
            // Increase lambda and continue searching
            lambda *= lambdaFactor;
          }
        }
      } // end while
    }
    gttoc(optimizer_iteration);
    //float diff( -((float)start-(float)clock())/CLOCKS_PER_SEC );
    //std::cout << "Iteration " << result.iterations << " takes time: " << std::setprecision(5) << diff*1.0e3 << "ms"<< std::endl;
    //std::cout << "Intermidiate Steps: " << result.intermediateSteps << std::endl;    
    //std::cout << "Current Errors: " << result.error << std::endl;

    result.iterations++;
  } while (result.iterations < maxIterations
      && !checkConvergence(relativeErrorTol, absoluteErrorTol, errorTol,
          previousError, result.error, NonlinearOptimizerParams::SILENT));
  
  //std::cout << "Iterations: " << result.iterations << std::endl;
  
  /*
  //std::cout << "started\n";
  GaussianFactorGraph linearFactorGraph = *factors_.linearize(theta_);
  // check degeneracy, ill-posed linear system
  for (auto it=linearFactorGraph.begin();it!=linearFactorGraph.end();it++) {
    if ( (*it)==NULL )
      continue;
    Matrix infoMat = (*it)->information();
    //svd
    // Check size of infoMat
    size_t n = infoMat.rows(), p = infoMat.cols(), m = min(n,p);
    // Do SVD on infoMat
    Eigen::JacobiSVD<Matrix> svd(infoMat, Eigen::ComputeFullV);
    Vector s = svd.singularValues();
    // Find rank
    size_t rank = 0;
    for (size_t j = 0; j < m; j++) {
      //std::cout << "asda " << s(j) << "\n";
      if (s(j) > (1e-9)) rank++;
    }
    // output
    //std::cout << "full rank is: "<< m <<" info matrix rank is: "<< rank << "\n"; 
  } 
  //std::cout << "passed\n";
  //*/
  return result;
}

/* ************************************************************************* */
void BatchFixedLagSmoother::marginalize(const KeyVector& marginalizeKeys) {
  // In order to marginalize out the selected variables, the factors involved in those variables
  // must be identified and removed. Also, the effect of those removed factors on the
  // remaining variables needs to be accounted for. This will be done with linear container factors
  // from the result of a partial elimination. This function removes the marginalized factors and
  // adds the linearized factors back in.

  // Identify all of the factors involving any marginalized variable. These must be removed.
  set<size_t> removedFactorSlots;
  const VariableIndex variableIndex(factors_);
  for(Key key: marginalizeKeys) {
    const FastVector<size_t>& slots = variableIndex[key];
    removedFactorSlots.insert(slots.begin(), slots.end());
  }

  // Add the removed factors to a factor graph
  NonlinearFactorGraph removedFactors;
  for(size_t slot: removedFactorSlots) {
    if (factors_.at(slot)) {
      removedFactors.push_back(factors_.at(slot));
    }
  }

  // Calculate marginal factors on the remaining keys
  NonlinearFactorGraph marginalFactors = CalculateMarginalFactors(
      removedFactors, theta_, marginalizeKeys, parameters_.getEliminationFunction());

  // Remove marginalized factors from the factor graph
  removeFactors(removedFactorSlots);

  // Remove marginalized keys from the system
  eraseKeys(marginalizeKeys);

  // Insert the new marginal factors
  insertFactors(marginalFactors);
}

/* ************************************************************************* */
void BatchFixedLagSmoother::PrintKeySet(const set<Key>& keys,
    const string& label) {
  cout << label;
  for(Key key: keys) {
    cout << " " << DefaultKeyFormatter(key);
  }
  cout << endl;
}

/* ************************************************************************* */
void BatchFixedLagSmoother::PrintKeySet(const KeySet& keys,
    const string& label) {
  cout << label;
  for(Key key: keys) {
    cout << " " << DefaultKeyFormatter(key);
  }
  cout << endl;
}

/* ************************************************************************* */
void BatchFixedLagSmoother::PrintSymbolicFactor(
    const NonlinearFactor::shared_ptr& factor) {
  cout << "f(";
  if (factor) {
    for(Key key: factor->keys()) {
      cout << " " << DefaultKeyFormatter(key);
    }
  } else {
    cout << " NULL";
  }
  cout << " )" << endl;
}

/* ************************************************************************* */
void BatchFixedLagSmoother::PrintSymbolicFactor(
    const GaussianFactor::shared_ptr& factor) {
  cout << "f(";
  for(Key key: factor->keys()) {
    cout << " " << DefaultKeyFormatter(key);
  }
  cout << " )" << endl;
}

/* ************************************************************************* */
void BatchFixedLagSmoother::PrintSymbolicGraph(
    const NonlinearFactorGraph& graph, const string& label) {
  cout << label << endl;
  for(const auto& factor: graph) {
    PrintSymbolicFactor(factor);
  }
}

/* ************************************************************************* */
void BatchFixedLagSmoother::PrintSymbolicGraph(const GaussianFactorGraph& graph,
    const string& label) {
  cout << label << endl;
  for(const auto& factor: graph) {
    PrintSymbolicFactor(factor);
  }
}

/* ************************************************************************* */
GaussianFactorGraph BatchFixedLagSmoother::CalculateMarginalFactors(
    const GaussianFactorGraph& graph, const KeyVector& keys,
    const GaussianFactorGraph::Eliminate& eliminateFunction) {
  if (keys.size() == 0) {
    // There are no keys to marginalize. Simply return the input factors
    return graph;
  } else {
    // .first is the eliminated Bayes tree, while .second is the remaining factor graph
    return *graph.eliminatePartialMultifrontal(keys, eliminateFunction).second;
  }
}

/* ************************************************************************* */
NonlinearFactorGraph BatchFixedLagSmoother::CalculateMarginalFactors(
    const NonlinearFactorGraph& graph, const Values& theta, const KeyVector& keys,
    const GaussianFactorGraph::Eliminate& eliminateFunction) {
  if (keys.size() == 0) {
    // There are no keys to marginalize. Simply return the input factors
    return graph;
  } else {
    // Create the linear factor graph
    const auto linearFactorGraph = graph.linearize(theta);

    const auto marginalLinearFactors =
        CalculateMarginalFactors(*linearFactorGraph, keys, eliminateFunction);

    // Wrap in nonlinear container factors
    return LinearContainerFactor::ConvertLinearGraph(marginalLinearFactors, theta);
  }
}

/* ************************************************************************* */
/// namespace gtsam
