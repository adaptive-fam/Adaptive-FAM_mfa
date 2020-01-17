//--------------------------------------------------------------
// mfa object
//
// Tom Peterka
// Argonne National Laboratory
// tpeterka@mcs.anl.gov
//--------------------------------------------------------------
#ifndef _MFA_HPP
#define _MFA_HPP

// comment out the following line for unclamped knots (single knot at each end of knot vector)
// clamped knots (repeated at ends) is the default method if no method is specified
// #define UNCLAMPED_KNOTS

// comment out the following line for domain parameterization
// domain parameterization is the default method if no method is specified
// #define CURVE_PARAMS

// comment out the following line for low-d knot insertion
// low-d is the default if no method is specified
// #define HIGH_D

// comment out the following line for applying weights to only the range dimension
// weighing the range coordinate only is the default if no method is specified
// #define WEIGH_ALL_DIMS

// comment out the following line for original single tensor product version
#define TMESH

#include    <Eigen/Dense>
#include    <vector>
#include    <list>
#include    <iostream>

#ifdef MFA_TBB
#include    <tbb/tbb.h>
using namespace tbb;
#endif

using namespace std;

using MatrixXf = Eigen::MatrixXf;
using VectorXf = Eigen::VectorXf;
using MatrixXd = Eigen::MatrixXd;
using VectorXd = Eigen::VectorXd;
using VectorXi = Eigen::VectorXi;
using ArrayXXf = Eigen::ArrayXXf;
using ArrayXXd = Eigen::ArrayXXd;
// NB, storing matrices and arrays in row-major order
template <typename T>
using MatrixX = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
template <typename T>
using VectorX  = Eigen::Matrix<T, Eigen::Dynamic, 1>;
template <typename T>
using ArrayXX  = Eigen::Array<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
template <typename T>
using ArrayX   = Eigen::Array<T, Eigen::Dynamic, 1>;

#include    <mfa/param.hpp>
#include    <mfa/tmesh.hpp>
#include    <mfa/mfa_data.hpp>
#include    <mfa/decode.hpp>
#include    <mfa/encode.hpp>

namespace mfa
{
    template <typename T>                           // float or double
    struct MFA
    {
        int                     dom_dim;            // domain dimensionality
        Param<T>*               mfa_param;          // pointer to parameterization object

        MFA(
            int                 dom_dim_,           // domain dimensionality (excluding science variables)
            const VectorXi&     ndom_pts_,          // number of input data points in each dim
            const MatrixX<T>&   domain_) :          // input data points (1st dim changes fastest)
            dom_dim(dom_dim_)
        {
            mfa_param = new Param<T>(dom_dim_, ndom_pts_, domain_);
        }

        ~MFA()
        {
            delete mfa_param;
        }

        VectorXi&               ndom_pts() const    { return mfa_param->ndom_pts; }
        vector<vector<T>>&      params() const      { return mfa_param->params; }
        vector<size_t>&         ds() const          { return mfa_param->ds; }
        vector<vector<size_t>>& co() const          { return mfa_param->co; }

        // fixed number of control points encode
        void FixedEncode(
                MFA_Data<T>&        mfa_data,               // mfa data model
                const MatrixX<T>&   domain,                 // input points
                const VectorXi      nctrl_pts,              // number of control points in each dim
                int                 verbose,                // debug level
                bool                weighted) const         // solve for and use weights (default = true)
        {
            // fixed encode assumes the tmesh has only one tensor product
            TensorProduct<T>&t = mfa_data.tmesh.tensor_prods[0];

            t.weights = VectorX<T>::Ones(t.nctrl_pts.prod());
            Encoder<T> encoder(*this, mfa_data, domain, verbose);
            encoder.Encode(t.nctrl_pts, t.ctrl_pts, t.weights, weighted);

            // debug: try inserting a knot
            //             VectorX<T> new_knot(mfa->dom_dim);
            //             for (auto i = 0; i < mfa->dom_dim; i++)
            //                 new_knot(i) = 0.5;
            //             mfa->KnotInsertion(new_knot, tmesh().tensor_prods[0]);
        }

        // adaptive encode
        void AdaptiveEncode(
                MFA_Data<T>&        mfa_data,               // mfa data model
                const MatrixX<T>&   domain,                 // input points
                T                   err_limit,              // maximum allowable normalized error
                int                 verbose,                // debug level
                bool                weighted,               // solve for and use weights (default = true)
                bool                local,                  // solve locally (with constraints) each round (default = false)
                const VectorX<T>&   extents,                // extents in each dimension, for normalizing error (size 0 means do not normalize)
                int                 max_rounds) const       // optional maximum number of rounds
        {
            Encoder<T> encoder(*this, mfa_data, domain, verbose);

#ifndef TMESH               // original adaptive encode for one tensor product
            if(local)
                encoder.OrigAdaptiveLocalEncode(err_limit, weighted, extents, max_rounds);
            else
                encoder.OrigAdaptiveEncode(err_limit, weighted, extents, max_rounds);
#else                       // adaptive encode for tmesh
            if(local)
                encoder.OrigAdaptiveLocalEncode(err_limit, weighted, extents, max_rounds);
            else
                encoder.AdaptiveEncode(err_limit, weighted, extents, max_rounds);
#endif
        }

        // decode values at all input points
        void DecodeDomain(
                const MFA_Data<T>&  mfa_data,               // mfa data model
                int                 verbose,                // debug level
                MatrixX<T>&         approx,                 // decoded points
                int                 min_dim,                // first dimension to decode
                int                 max_dim,                // last dimension to decode
                bool                saved_basis) const      // whether basis functions were saved and can be reused
        {
            VectorXi no_derivs;                             // size-0 means no derivatives
            DecodeDomain(mfa_data, verbose, approx, min_dim, max_dim, saved_basis, no_derivs);
        }

        // decode derivatives at all input points
        void DecodeDomain(
                const MFA_Data<T>&  mfa_data,               // mfa data model
                int                 verbose,                // debug level
                MatrixX<T>&         approx,                 // decoded values
                int                 min_dim,                // first dimension to decode
                int                 max_dim,                // last dimension to decode
                bool                saved_basis,            // whether basis functions were saved and can be reused
                const VectorXi&     derivs) const           // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                            // pass size-0 vector if unused
        {
            mfa::Decoder<T> decoder(*this, mfa_data, verbose);
            decoder.DecodeDomain(approx, min_dim, max_dim, saved_basis, derivs);
        }

        // decode single point at the given parameter location
        void DecodePt(
                const MFA_Data<T>&  mfa_data,               // mfa data model
                const VectorX<T>&   param,                  // parameters of point to decode
                VectorX<T>&         cpt) const              // (output) decoded point
        {
            int verbose = 0;
            Decoder<T> decoder(*this, mfa_data, verbose);
            // TODO: hard-coded for one tensor product
            decoder.VolPt(param, cpt, mfa_data.tmesh.tensor_prods[0]);
        }

        // compute the error (absolute value of coordinate-wise difference) of the mfa at a domain point
        // error is not normalized by the data range (absolute, not relative error)
        void AbsCoordError(
                const MFA_Data<T>&  mfa_data,               // mfa data model
                const MatrixX<T>&   domain,                 // input points
                size_t              idx,                    // index of domain point
                VectorX<T>&         error,                  // (output) absolute value of error at each coordinate
                int                 verbose) const          // debug level
        {
            // convert linear idx to multidim. i,j,k... indices in each domain dimension
            VectorXi ijk(dom_dim);
            mfa_data.idx2ijk(ds(), idx, ijk);

            // compute parameters for the vertices of the cell
            VectorX<T> param(dom_dim);
            for (int i = 0; i < dom_dim; i++)
                param(i) = mfa_param->params[i][ijk(i)];

            // NB, assumes at least one tensor product exists and that all have the same ctrl pt dimensionality
            int pt_dim = mfa_data.tmesh.tensor_prods[0].ctrl_pts.cols();

            // approximated value
            VectorX<T> cpt(pt_dim);          // approximated point
            Decoder<T> decoder(*this, mfa_data, verbose);
            decoder.VolPt(param, cpt, mfa_data.tmesh.tensor_prods[0]);      // TODO: hard-coded for first tensor product

            for (auto i = 0; i < pt_dim; i++)
                error(i) = fabs(cpt(i) - domain(idx, mfa_data.min_dim + i));
        }
    };
}                                           // namespace

#endif
