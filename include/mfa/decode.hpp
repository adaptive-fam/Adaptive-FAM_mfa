//--------------------------------------------------------------
// decoder object
//
// Tom Peterka
// Argonne National Laboratory
// tpeterka@mcs.anl.gov
//--------------------------------------------------------------

#ifndef _DECODE_HPP
#define _DECODE_HPP

#include    "mfa_data.hpp"
#include    "mfa.hpp"

#include    <Eigen/Dense>

typedef Eigen::MatrixXi MatrixXi;

namespace mfa
{
    template <typename T>                                   // float or double
    struct MFA;

    template <typename T>
    class Decoder;

    template <typename T>                                   // float or double
    struct DecodeInfo
    {
        vector<MatrixX<T>>  N;                              // basis functions in each dim.
        vector<VectorX<T>>  temp;                           // temporary point in each dim.
        vector<int>         span;                           // current knot span in each dim.
        vector<int>         n;                              // number of control point spans in each dim
        vector<int>         iter;                           // iteration number in each dim.
        VectorX<T>          ctrl_pt;                        // one control point
        int                 ctrl_idx;                       // control point linear ordering index
        VectorX<T>          temp_denom;                     // temporary rational NURBS denominator in each dim
        vector<MatrixX<T>>  ders;                           // derivatives in each dim.

        DecodeInfo(const MFA_Data<T>&   mfa_data,           // current mfa
                   const VectorXi&      derivs)             // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                            // pass size-0 vector if unused
        {
            N.resize(mfa_data.p.size());
            temp.resize(mfa_data.p.size());
            span.resize(mfa_data.p.size());
            n.resize(mfa_data.p.size());
            iter.resize(mfa_data.p.size());
            ctrl_pt.resize(mfa_data.tmesh.tensor_prods[0].ctrl_pts.cols());
            temp_denom = VectorX<T>::Zero(mfa_data.p.size());
            ders.resize(mfa_data.p.size());

            for (size_t i = 0; i < mfa_data.dom_dim; i++)
            {
                temp[i]    = VectorX<T>::Zero(mfa_data.tmesh.tensor_prods[0].ctrl_pts.cols());
                // TODO: hard-coded for one tensor product
                N[i]       = MatrixX<T>::Zero(1, mfa_data.tmesh.tensor_prods[0].nctrl_pts(i));
                if (derivs.size() && derivs(i))
                    // TODO: hard-coded for one tensor product
                    ders[i] = MatrixX<T>::Zero(derivs(i) + 1, mfa_data.tmesh.tensor_prods[0].nctrl_pts(i));
            }
        }

        // reset decode info
        // version for recomputing basis functions
        void Reset(const MFA_Data<T>&   mfa_data,           // current mfa
                   const VectorXi&      derivs)             // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                            // pass size-0 vector if unused
        {
            temp_denom.setZero();
            for (auto i = 0; i < mfa_data.dom_dim; i++)
            {
                temp[i].setZero();
                iter[i] = 0;
                N[i].setZero();
                if (derivs.size() && derivs(i))
                    ders[i].setZero();
            }
        }

        // reset decode info
        // version for saved basis functions
        void Reset_saved_basis(const MFA_Data<T>&   mfa_data)    // current mfa
        {
            temp_denom.setZero();
            for (auto i = 0; i < mfa_data.dom_dim; i++)
            {
                temp[i].setZero();
                iter[i] = 0;
            }
        }
    };

    // Custom DecodeInfo to be used with FastVolPt, FastGrad
    template <typename T>
    struct FastDecodeInfo
    {
        const Decoder<T>&   decoder;            // reference to decoder which uses this FastDecodeInfo
        BasisFunInfo<T>     bfi;                // struct with pre-allocated scratch space for basis function computation
        const int           dom_dim;            // domain dimension of model

        vector<vector<T>>           N;          // stores basis functions
        vector<vector<vector<T>>>   D;          // stores derivatives of basis functions
        T***                        M;          // aliases D for faster iteration in FastGrad
        vector<vector<T>>           t;          // stores intermediate sums from k-mode vector products
        vector<vector<vector<T>>>   td;         // stores intermediate sums from k-mode vector products (FastGrad version)

        int                         nders;      // number of derivatives currently supported by D & M
        vector<int>                 span;       // vector to hold spans which contain the given parameter
        int                         ctrl_idx;   // index of the current control point

        FastDecodeInfo(const Decoder<T>& decoder_) :
            decoder(decoder_),
            dom_dim(decoder.dom_dim),
            nders(0),
            M(nullptr),
            bfi(decoder.q)
        {
            N.resize(dom_dim);
            span.resize(dom_dim);

            for (size_t i = 0; i < dom_dim; i++)
            {
                N[i].resize(decoder.q[i], 0);
            }

            // t and td are multi-dim arrays containing intermediate sums formed by k-mode vector products
            t.resize(dom_dim);
            td.resize(dom_dim+1);            
            for (int i = 0; i < dom_dim - 1; i++)
            {
                t[i].resize(decoder.tot_iters / decoder.ds[i+1]);

            }
            t[dom_dim-1].resize(1);

            for (int d = 0; d < dom_dim + 1; d++)
            {
                td[d].resize(dom_dim);
                for (int i = 0; i < dom_dim - 1; i++)
                {
                    td[d][i].resize(decoder.tot_iters / decoder.ds[i+1]);
                }
                td[d][dom_dim-1].resize(1);
            }
        }

        ~FastDecodeInfo()
        {
            DeleteM();
        }

        void DeleteM()
        {
            if (M == nullptr)
                return;
            else
            {
                for (int d = 0; d < dom_dim + 1; d++)
                {
                    //Note: We do NOT want to delete M[d][k], since this points to some vector which is managed elsewhere
                    delete[] M[d];
                    M[d] = nullptr;
                }
                delete[] M;
                M = nullptr;
            }
        }

        // D is a 3d array holding the derivatives of each basis function in each dimension.
        // For D[k][d][i],
        //      k = dimension in parameter space
        //      d = derivative order (0 = value)
        //      i = index of basis function, i = 0,...,p
        //     D.size() = dom_dim;  and D[d].size() = nders+1 for each d
        //
        // M is a 3d array with DIFFERENT semantic meaning of indices.
        // M[d] contains the values to multiply in order to compute the derivative of a tensor-product basis function in the dth direction
        //      M[d][d] is an array of the (nder)^th derivatives of basis functions in direction d
        //      M[d][k] is an array of the basis functions in direction k (when d != k)
        // The idea is that 
        //      M[d][0][i] * M[d][1][i] * ... * M[d][dom_dim-1][i]
        // will always be the value of the derivative of a tensor-product basis function in the d^th direction.
        // Thus no "if" logic is needed to switch between multiplying by values or by derivatives in FastGrad, etc
        // 
        // M is a collection of pointers which aliases D; we do not want to move any actual basis function values into M
        void ResizeDers(int nders)
        {
            D.resize(dom_dim);
            for (int k = 0; k < dom_dim; k++)
            {
                D[k].resize(nders + 1);
                for (int d = 0; d < nders + 1; d++)
                {
                    D[k][d].resize(decoder.q[k], 0);
                }
            }

            // Reset alias matrix M
            DeleteM();
            M = new T**[dom_dim+1];
            for (int d = 0; d < dom_dim + 1; d++)
            {
                M[d] = new T*[dom_dim];
                for (int k = 0; k < dom_dim; k++)
                {
                    if (k == d)
                    {
                        M[d][k] = &(D[k][nders][0]);    // point to start of D[k][nders]
                    }
                    else
                    {
                        M[d][k] = &(D[k][0][0]);        // point to start of D[k][0]
                    }
                }
            }
        }

        // reset fast decode info
        void Reset()
        {
            // FastVolPt, FastGrad do not require FastDecodeInfo to be reset as written
        }
    };


    template <typename T>                               // float or double
    class Decoder
    {
        friend FastDecodeInfo<T>;

    private:
        const MFA_Data<T>&  mfa_data;                   // the mfa data model
        const int           dom_dim;
        const int           tot_iters;                  // total iterations in flattened decoding of all dimensions
        MatrixXi            ct;                         // coordinates of first control point of curve for given iteration
                                                        // of decoding loop, relative to start of box of
                                                        // control points
        VectorXi            cs;                         // control point stride (only in decoder, not mfa)
        vector<int>         ds; // subvolume stride
        VectorXi            jumps;                      // total jump in index from start ctrl_idx for each ctrl point
        int                 q0;                         // p+1 in first dimension (used for FastVolPt)
        vector<int>         q;

        int                 verbose;                    // output level
        bool                saved_basis;                // flag to use saved basis functions within mfa_data

    public:

        Decoder(
                const MFA_Data<T>&  mfa_data_,              // MFA data model
                int                 verbose_,               // debug level
                bool                saved_basis_=false) :   // flag to reuse saved basis functions   
            mfa_data(mfa_data_),
            dom_dim(mfa_data_.dom_dim),
            tot_iters((mfa_data.p + VectorXi::Ones(dom_dim)).prod()),
            q0(mfa_data_.p(0)+1),
            verbose(verbose_),
            saved_basis(saved_basis_)
        {
            // ensure that encoding was already done
            if (!mfa_data.p.size()                               ||
                !mfa_data.tmesh.all_knots.size()                 ||
                !mfa_data.tmesh.tensor_prods.size()              ||
                !mfa_data.tmesh.tensor_prods[0].nctrl_pts.size() ||
                !mfa_data.tmesh.tensor_prods[0].ctrl_pts.size())
            {
                fprintf(stderr, "Decoder() error: Attempting to decode before encoding.\n");
                exit(0);
            }

            // initialize decoding data structures
            // TODO: hard-coded for first tensor product only
            // needs to be expanded for multiple tensor products, maybe moved into the tensor product
            cs = VectorXi::Ones(mfa_data.dom_dim);
            ds.resize(dom_dim, 1);
            q.resize(dom_dim);
            for (size_t i = 0; i < mfa_data.p.size(); i++)   // for all dims
            {
                q[i] = mfa_data.p(i) + 1;
                if (i > 0)
                {
                    cs[i] = cs[i - 1] * mfa_data.tmesh.tensor_prods[0].nctrl_pts[i - 1];
                    ds[i] = ds[i-1] * q[i];
                }
            }
            ct.resize(tot_iters, mfa_data.p.size());

            // compute coordinates of first control point of curve corresponding to this iteration
            // these are relative to start of the box of control points located at co
            for (int i = 0; i < tot_iters; i++)      // 1-d flattening all n-d nested loop computations
            {
                int div = tot_iters;
                int i_temp = i;
                for (int j = mfa_data.p.size() - 1; j >= 0; j--)
                {
                    div      /= (mfa_data.p(j) + 1);
                    ct(i, j) =  i_temp / div;
                    i_temp   -= (ct(i, j) * div);
                }
            }

            jumps = ct * cs;
        }

        ~Decoder() {}

        // computes approximated points from a given set of parameter values  and an n-d NURBS volume
        // P&T eq. 9.77, p. 424
        // assumes ps contains parameter values to decode at; 
        // decoded points store in ps
        void DecodePointSet(
                        PointSet<T>&    ps,         // PointSet containing parameters to decode at
                        int             min_dim,    // first dimension to decode
                        int             max_dim)    // last dimension to decode
        {
            VectorXi no_ders;                       // size 0 means no derivatives
            DecodePointSet(ps, min_dim, max_dim, no_ders);
        }

        // computes approximated points from a given set of parameter values  and an n-d NURBS volume
        // P&T eq. 9.77, p. 424
        // assumes ps contains parameter values to decode at; 
        // decoded points store in ps
        void DecodePointSet(
                        PointSet<T>&    ps,         // PointSet containing parameters to decode at
                        int             min_dim,    // first dimension to decode
                        int             max_dim,    // last dimension to decode
                const   VectorXi&       derivs)     // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                    // pass size-0 vector if unused
        {
            if (saved_basis && !ps.structured)
                cerr << "Warning: Saved basis decoding not implemented with unstructured input. Proceeding with standard decoding" << endl;

            int last = mfa_data.tmesh.tensor_prods[0].ctrl_pts.cols() - 1;       // last coordinate of control point

#ifdef MFA_TBB                                          // TBB version, faster (~3X) than serial
            // thread-local DecodeInfo
            // ref: https://www.threadingbuildingblocks.org/tutorial-intel-tbb-thread-local-storage
            enumerable_thread_specific<DecodeInfo<T>> thread_decode_info(mfa_data, derivs);

            static affinity_partitioner ap;
            parallel_for (blocked_range<size_t>(0, ps.npts), [&](blocked_range<size_t>& r)
            {
                auto pt_it  = ps.iterator(r.begin());
                auto pt_end = ps.iterator(r.end());
                for (; pt_it != pt_end; ++pt_it)
                {
                    VectorX<T>  cpt(last + 1);              // evaluated point
                    VectorX<T>  param(mfa_data.dom_dim);    // vector of param values
                    VectorXi    ijk(mfa_data.dom_dim);      // vector of param indices (structured grid only)
                    pt_it.params(param);
                    // compute approximated point for this parameter vector

#ifndef MFA_TMESH   // original version for one tensor product

                    if (saved_basis && ps.structured)
                    {
                        pt_it.ijk(ijk);
                        VolPt_saved_basis(ijk, param, cpt, thread_decode_info.local(), mfa_data.tmesh.tensor_prods[0]);

                        // debug
                        if (pt_it.idx() == 0)
                            fprintf(stderr, "Using VolPt_saved_basis\n");
                    }
                    else
                    {
                        VolPt(param, cpt, thread_decode_info.local(), mfa_data.tmesh.tensor_prods[0], derivs);

                        // debug
                        if (pt_it.idx() == 0)
                            fprintf(stderr, "Using VolPt\n");
                    }

#else           // tmesh version

                    if (pt_it.idx() == 0)
                        fprintf(stderr, "Using VolPt_tmesh\n");
                    VolPt_tmesh(param, cpt);

#endif

                    ps.domain.block(pt_it.idx(), min_dim, 1, max_dim - min_dim + 1) = cpt.transpose();
                }
            }, ap);
            if (verbose)
                fprintf(stderr, "100 %% decoded\n");

#endif              // end TBB version

#ifdef MFA_SERIAL   // serial version
            DecodeInfo<T> decode_info(mfa_data, derivs);    // reusable decode point info for calling VolPt repeatedly

            VectorX<T> cpt(last + 1);                       // evaluated point
            VectorX<T> param(mfa_data.dom_dim);            // parameters for one point
            VectorXi   ijk(mfa_data.dom_dim);      // vector of param indices (structured grid only)

            auto pt_it  = ps.begin();
            auto pt_end = ps.end();
            for (; pt_it != pt_end; ++pt_it)
            {
                // Get parameter values and indices at current point
                pt_it.params(param);

                // compute approximated point for this parameter vector

#ifndef MFA_TMESH   // original version for one tensor product

                if (saved_basis && ps.structured)
                {
                    pt_it.ijk(ijk);
                    VolPt_saved_basis(ijk, param, cpt, decode_info, mfa_data.tmesh.tensor_prods[0]);

                    // debug
                    if (pt_it.idx() == 0)
                        fprintf(stderr, "Using VolPt_saved_basis\n");
                }
                else
                {
                    // debug
                    if (pt_it.idx() == 0)
                        fprintf(stderr, "Using VolPt\n");

                    VolPt(param, cpt, decode_info, mfa_data.tmesh.tensor_prods[0], derivs);
                }

#else           // tmesh version

                if (pt_it.idx() == 0)
                    fprintf(stderr, "Using VolPt_tmesh\n");
                VolPt_tmesh(param, cpt);

#endif          // end serial version

                ps.domain.block(pt_it.idx(), min_dim, 1, max_dim - min_dim + 1) = cpt.transpose();

                // print progress
                if (verbose)
                    if (pt_it.idx() > 0 && ps.npts >= 100 && pt_it.idx() % (ps.npts / 100) == 0)
                        fprintf(stderr, "\r%.0f %% decoded", (T)pt_it.idx() / (T)(ps.npts) * 100);
            }

#endif
        }

        // decode at a regular grid using saved basis that is computed once by this function
        // and then used to decode all the points in the grid
        void DecodeGrid(MatrixX<T>&         result,         // output
                        int                 min_dim,        // min dimension to decode
                        int                 max_dim,        // max dimension to decode
                        const VectorX<T>&   min_params,     // lower corner of decoding points
                        const VectorX<T>&   max_params,     // upper corner of decoding points
                        const VectorXi&     ndom_pts)       // number of points to decode in each direction
        {
            // precompute basis functions
            const VectorXi&     nctrl_pts = mfa_data.tmesh.tensor_prods[0].nctrl_pts;   // reference to control points (assume only one tensor)

            Param<T> full_params(ndom_pts, min_params, max_params);

            // TODO: Eventually convert "result" into a PointSet and iterate through that,
            //       instead of simply using a naked Param object  
            auto& params = full_params.param_grid;


            // compute basis functions for points to be decoded
            vector<MatrixX<T>>  NN(mfa_data.dom_dim);
            for (int k = 0; k < mfa_data.dom_dim; k++)
            {
                NN[k] = MatrixX<T>::Zero(ndom_pts(k), nctrl_pts(k));

                for (int i = 0; i < NN[k].rows(); i++)
                {
                    int span = mfa_data.FindSpan(k, params[k][i], nctrl_pts(k));
#ifndef MFA_TMESH   // original version for one tensor product

                    mfa_data.OrigBasisFuns(k, params[k][i], span, NN[k], i);

#else               // tmesh version

                    // TODO: TBD

#endif              // tmesh version
                }
            }

            VectorXi    derivs;                             // do not use derivatives yet, pass size 0
            VolIterator vol_it(ndom_pts);

#ifdef MFA_SERIAL   // serial version

            DecodeInfo<T>   decode_info(mfa_data, derivs);  // reusable decode point info for calling VolPt repeatedly
            VectorX<T>      cpt(mfa_data.tmesh.tensor_prods[0].ctrl_pts.cols());                      // evaluated point
            VectorX<T>      param(mfa_data.dom_dim);       // parameters for one point
            VectorXi        ijk(mfa_data.dom_dim);          // multidim index in grid

            while (!vol_it.done())
            {
                int j = (int) vol_it.cur_iter();
                for (auto i = 0; i < mfa_data.dom_dim; i++)
                {
                    ijk[i] = vol_it.idx_dim(i);             // index along direction i in grid
                    param(i) = params[i][ijk[i]];
                }

#ifndef MFA_TMESH   // original version for one tensor product

                VolPt_saved_basis_grid(ijk, param, cpt, decode_info, mfa_data.tmesh.tensor_prods[0], NN);

#else               // tmesh version

                    // TODO: TBD

#endif              // tmesh version

                vol_it.incr_iter();
                result.block(j, min_dim, 1, max_dim - min_dim + 1) = cpt.transpose();
            }

#endif              // serial version

#ifdef MFA_TBB      // TBB version

            // thread-local objects
            // ref: https://www.threadingbuildingblocks.org/tutorial-intel-tbb-thread-local-storage
            enumerable_thread_specific<DecodeInfo<T>>   thread_decode_info(mfa_data, derivs);
            enumerable_thread_specific<VectorXi>        thread_ijk(mfa_data.dom_dim);              // multidim index in grid
            enumerable_thread_specific<VectorX<T>>      thread_cpt(mfa_data.tmesh.tensor_prods[0].ctrl_pts.cols());                      // evaluated point
            enumerable_thread_specific<VectorX<T>>      thread_param(mfa_data.p.size());           // parameters for one point

            parallel_for (size_t(0), (size_t)vol_it.tot_iters(), [&] (size_t j)
            {
                vol_it.idx_ijk(j, thread_ijk.local());
                for (auto i = 0; i < mfa_data.dom_dim; i++)
                    thread_param.local()(i)    = params[i][thread_ijk.local()[i]];

#ifndef MFA_TMESH   // original version for one tensor product

                VolPt_saved_basis_grid(thread_ijk.local(), thread_param.local(), thread_cpt.local(), thread_decode_info.local(), mfa_data.tmesh.tensor_prods[0], NN);

#else           // tmesh version

                // TODO: TBD

#endif          // tmesh version

                result.block(j, min_dim, 1, max_dim - min_dim + 1) = thread_cpt.local().transpose();
            });

#endif      // TBB version

        }

        // decode a point in the t-mesh
        // TODO: serial implementation, no threading
        // TODO: no derivatives as yet
        // TODO: weighs all dims, whereas other versions of VolPt have a choice of all dims or only last dim
        void VolPt_tmesh(const VectorX<T>&      param,      // parameters of point to decode
                         VectorX<T>&            out_pt)     // (output) point, allocated by caller
        {
            // debug
//             cerr << "VolPt_tmesh(): decoding point with param: " << param.transpose() << endl;

            // debug
            bool debug = false;
//             if (fabs(param(0) - 0.010101) < 1e-3 && fabs(param(1) - 0.0) < 1e-3)
//                 debug = true;

            // init
            out_pt = VectorX<T>::Zero(out_pt.size());
            T B_sum = 0.0;                                                          // sum of multidim basis function products
            T w_sum = 0.0;                                                          // sum of control point weights

            // compute range of anchors covering decoded point
            vector<vector<KnotIdx>> anchors(mfa_data.dom_dim);

            //             DEPRECATE using the 'expand' argument in anchors()
//             mfa_data.tmesh.anchors(param, true, anchors);
            mfa_data.tmesh.anchors(param, anchors);

            for (auto k = 0; k < mfa_data.tmesh.tensor_prods.size(); k++)           // for all tensor products
            {
                const TensorProduct<T>& t = mfa_data.tmesh.tensor_prods[k];

                // debug
                if (debug)
                {
                    fmt::print(stderr, "VolPt_tmesh(): tensor {}\n", k);
                    for (auto j = 0; j < mfa_data.dom_dim; j++)
                        fmt::print(stderr, "anchors[{}] = [{}]\n", j, fmt::join(anchors[j], ","));
                }

                // skip entire tensor if knot mins, maxs are too far away from decoded point
                bool skip = false;
                for (auto j = 0; j < mfa_data.dom_dim; j++)
                {
                    if (t.knot_maxs[j] < anchors[j].front() || t.knot_mins[j] > anchors[j].back())
                    {
                        // debug
                        if (debug)
                            cerr << "Skipping tensor " << k << " when decoding point param " << param.transpose() << endl;

                        skip = true;
                        break;
                    }
                }

                if (skip)
                    continue;

                VolIterator         vol_iterator(t.nctrl_pts);                      // iterator over control points in the current tensor
                vector<KnotIdx>     anchor(mfa_data.dom_dim);                       // one anchor in (global, ie, over all tensors) index space
                VectorXi            ijk(mfa_data.dom_dim);                          // multidim index of current control point

                while (!vol_iterator.done())
                {
                    // get anchor of the current control point
                    vol_iterator.idx_ijk(vol_iterator.cur_iter(), ijk);
                    mfa_data.tmesh.ctrl_pt_anchor(t, ijk, anchor);

                    // skip odd degree duplicated control points, indicated by invalid weight
                    if (t.weights(vol_iterator.cur_iter()) == MFA_NAW)
                    {
                        // debug
//                         if (debug)
//                             cerr << "skipping ctrl pt (MFA_NAW) " << t.ctrl_pts.row(vol_iterator.cur_iter()) << endl;

                        vol_iterator.incr_iter();
                        continue;
                    }

                    // debug
//                     bool skip = false;

                    // skip control points too far away from the decoded point
                    if (!mfa_data.tmesh.in_anchors(anchor, anchors))
                    {
                        // debug
//                         if (debug)
//                         {
//                             cerr << "skipping ctrl pt (too far away) [" << ijk.transpose() << "] " << t.ctrl_pts.row(vol_iterator.cur_iter()) << endl;
//                             fmt::print(stderr, "anchor [{}]\n", fmt::join(anchor, ","));
//                         }

                        // debug
//                         if (debug)
//                             skip = true;

                        // debug
//                         skip = true;

                        vol_iterator.incr_iter();
                        continue;
                    }

                    // debug
//                     if (debug)
//                         fmt::print(stderr, "VolPt_tmesh() calling knot_intersections w/ anchor [{}]\n", fmt::join(anchor, ","));

                    // intersect tmesh lines to get local knot indices in all directions
                    vector<vector<KnotIdx>> local_knot_idxs(mfa_data.dom_dim);          // local knot vectors in index space
                    mfa_data.tmesh.knot_intersections(anchor, k, true, local_knot_idxs);

                    // debug
//                     if (debug)
//                     {
//                         fmt::print(stderr, "VolPt_tmesh(): anchor [{}] ", fmt::join(anchor, ","));
//                         for (auto j = 0; j < mfa_data.dom_dim; j++)
//                             fmt::print(stderr, "local_knot_idxs[{}] [{}] ", j, fmt::join(local_knot_idxs[j], ","));
//                         fmt::print(stderr, "\n");
//                     }

                    // compute product of basis functions in each dimension
                    T B = 1.0;                                                          // product of basis function values in each dimension
                    for (auto i = 0; i < mfa_data.dom_dim; i++)
                    {
                        vector<T> local_knots(mfa_data.p(i) + 2);                       // local knot vector for current dim in parameter space
                        for (auto n = 0; n < local_knot_idxs[i].size(); n++)
                            local_knots[n] = mfa_data.tmesh.all_knots[i][local_knot_idxs[i][n]];

                        B *= mfa_data.OneBasisFun(i, param(i), local_knots);
                    }

                    // compute the point
                    out_pt += B * t.ctrl_pts.row(vol_iterator.cur_iter()) * t.weights(vol_iterator.cur_iter());

                    // debug
//                     if (skip && B != 0.0)
//                     {
//                         cerr << "\nVolPt_tmesh(): Error: incorrect skip. decoding point with param: " << param.transpose() << endl;
//                         cerr << "tensor " << k << " skipping ctrl pt [" << ijk.transpose() << "] " << endl;
//                         fmt::print(stderr, "anchor [{}]\n", fmt::join(anchor, ","));
//                     }

                    B_sum += B * t.weights(vol_iterator.cur_iter());

                    vol_iterator.incr_iter();                                           // must increment volume iterator at the bottom of the loop
                }       // volume iterator
            }       // tensors

            // debug
//             if (debug)
//                 cerr << "out_pt: " << out_pt.transpose() << " B_sum: " << B_sum << "\n" << endl;

            // divide by sum of weighted basis functions to make a partition of unity
            if (B_sum > 0.0)
                out_pt /= B_sum;
            else
                cerr << "Warning: VolPt_tmesh(): B_sum = 0 when decoding param: " << param.transpose() << " This should not happen." << endl;
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // slower version for single points
        void VolPt(
                const VectorX<T>&       param,      // parameter value in each dim. of desired point
                VectorX<T>&             out_pt,     // (output) point, allocated by caller
                const TensorProduct<T>& tensor)     // tensor product to use for decoding
        {
            VectorXi no_ders;                   // size 0 vector means no derivatives
            VolPt(param, out_pt, tensor, no_ders);
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // faster version for multiple points, reuses decode info
        void VolPt(
                const VectorX<T>&       param,      // parameter value in each dim. of desired point
                VectorX<T>&             out_pt,     // (output) point, allocated by caller
                DecodeInfo<T>&          di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                const TensorProduct<T>& tensor)     // tensor product to use for decoding
        {
            VectorXi no_ders;                   // size 0 vector means no derivatives
            VolPt(param, out_pt, di, tensor, no_ders);
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // slower version for single points
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt(
                const VectorX<T>&       param,      // parameter value in each dim. of desired point
                VectorX<T>&             out_pt,     // (output) point, allocated by caller
                const TensorProduct<T>& tensor,     // tensor product to use for decoding
                const VectorXi&         derivs)     // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                    // pass size-0 vector if unused
        {
            int last = mfa_data.tmesh.tensor_prods[0].ctrl_pts.cols() - 1;      // last coordinate of control point
            if (derivs.size())                                                  // extra check for derivatives, won't slow down normal point evaluation
            {
                if (derivs.size() != mfa_data.p.size())
                {
                    fprintf(stderr, "Error: size of derivatives vector is not the same as the number of domain dimensions\n");
                    exit(0);
                }
                for (auto i = 0; i < mfa_data.p.size(); i++)
                    if (derivs(i) > mfa_data.p(i))
                        fprintf(stderr, "Warning: In dimension %d, trying to take derivative %d of an MFA with degree %d will result in 0. This may not be what you want",
                                i, derivs(i), mfa_data.p(i));
            }

            // init
            vector <MatrixX<T>> N(mfa_data.p.size());                           // basis functions in each dim.
            vector<VectorX<T>>  temp(mfa_data.p.size());                        // temporary point in each dim.
            vector<int>         span(mfa_data.p.size());                        // span in each dim.
            VectorX<T>          ctrl_pt(last + 1);                              // one control point
            int                 ctrl_idx;                                       // control point linear ordering index
            VectorX<T>          temp_denom = VectorX<T>::Zero(mfa_data.p.size());// temporary rational NURBS denominator in each dim

            // set up the volume iterator
            VectorXi npts = mfa_data.p + VectorXi::Ones(mfa_data.dom_dim);      // local support is p + 1 in each dim.
            VolIterator vol_iter(npts);                                         // for iterating in a flat loop over n dimensions

            // basis funs
            for (size_t i = 0; i < mfa_data.dom_dim; i++)                       // for all dims
            {
                temp[i]    = VectorX<T>::Zero(last + 1);
                span[i]    = mfa_data.FindSpan(i, param(i), tensor);
                N[i]       = MatrixX<T>::Zero(1, tensor.nctrl_pts(i));
                if (derivs.size() && derivs(i))
                {
#ifndef MFA_TMESH   // original version for one tensor product
                    MatrixX<T> Ders = MatrixX<T>::Zero(derivs(i) + 1, tensor.nctrl_pts(i));
                    mfa_data.DerBasisFuns(i, param(i), span[i], derivs(i), Ders);
                    N[i].row(0) = Ders.row(derivs(i));
#endif
                }
                else
                {
#ifndef MFA_TMESH   // original version for one tensor product
                    mfa_data.OrigBasisFuns(i, param(i), span[i], N[i], 0);
#else               // tmesh version
                    mfa_data.BasisFuns(i, param(i), span[i], N[i], 0);
#endif
                }
            }

            // linear index of first control point
            ctrl_idx = 0;
            for (int j = 0; j < mfa_data.p.size(); j++)
                ctrl_idx += (span[j] - mfa_data.p(j) + ct(0, j)) * cs[j];
            size_t start_ctrl_idx = ctrl_idx;

            while (!vol_iter.done())
            {
                // always compute the point in the first dimension
                ctrl_pt = tensor.ctrl_pts.row(ctrl_idx);
                T w     = tensor.weights(ctrl_idx);

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
                temp[0] += (N[0])(0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0)) * ctrl_pt * w;
#else                                                                           // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (temp[0])(j) += (N[0])(0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0)) * ctrl_pt(j);
                (temp[0])(last) += (N[0])(0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0)) * ctrl_pt(last) * w;
#endif

                temp_denom(0) += w * N[0](0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0));

                vol_iter.incr_iter();                                           // must call near bottom of loop, but before checking for done span below

                // for all dimensions except last, check if span is finished
                ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa_data.p.size(); k++)
                {
                    if (vol_iter.cur_iter() < vol_iter.tot_iters())
                        ctrl_idx += ct(vol_iter.cur_iter(), k) * cs[k];         // ctrl_idx for the next iteration
                    if (k < mfa_data.dom_dim - 1 && vol_iter.done(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        // use prev_idx_dim because iterator was already incremented above
                        temp[k + 1]        += (N[k + 1])(0, vol_iter.prev_idx_dim(k + 1) + span[k + 1] - mfa_data.p(k + 1)) * temp[k];
                        temp_denom(k + 1)  += temp_denom(k) * N[k + 1](0, vol_iter.prev_idx_dim(k + 1) + span[k + 1] - mfa_data.p(k + 1));
                        temp_denom(k)       = 0.0;
                        temp[k]             = VectorX<T>::Zero(last + 1);
                    }
                }
            }

            T denom;                                                            // rational denominator
            if (derivs.size() && derivs.sum())
                denom = 1.0;                                                    // TODO: weights for derivatives not implemented yet
            else
                denom = temp_denom(mfa_data.p.size() - 1);

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
            out_pt = temp[mfa_data.p.size() - 1] / denom;
#else                                                                           // weigh only range dimension
            out_pt   = temp[mfa_data.p.size() - 1];
            out_pt(last) /= denom;
#endif

        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // slower version for single points
        // explicit full set of control points and weights
        // used only for testing tmesh during development (deprecate/remove eventually)
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt(
                const VectorX<T>&       param,              // parameter value in each dim. of desired point
                VectorX<T>&             out_pt,             // (output) point, allocated by caller
                const VectorXi&         nctrl_pts,          // number of control points
                const MatrixX<T>&       ctrl_pts,           // p+1 control points per dimension, linearized
                const VectorX<T>&       weights)            // p+1 weights per dimension, linearized
        {
            int last = ctrl_pts.cols() - 1;                 // last coordinate of control point

            // init
            vector <MatrixX<T>> N(mfa_data.p.size());       // basis functions in each dim.
            vector<VectorX<T>>  temp(mfa_data.p.size());    // temporary point in each dim.
            vector<int>         span(mfa_data.p.size());    // span in each dim.
            VectorX<T>          ctrl_pt(last + 1);          // one control point
            int                 ctrl_idx;                   // control point linear ordering index
            VectorX<T>          temp_denom = VectorX<T>::Zero(mfa_data.p.size());     // temporary rational NURBS denominator in each dim

            // set up the volume iterator
            VectorXi npts = mfa_data.p + VectorXi::Ones(mfa_data.dom_dim);      // local support is p + 1 in each dim.
            VolIterator vol_iter(npts);                                         // for iterating in a flat loop over n dimensions

            // basis funs
            for (size_t i = 0; i < mfa_data.dom_dim; i++)   // for all dims
            {
                temp[i]    = VectorX<T>::Zero(last + 1);
                span[i]    = mfa_data.FindSpan(i, param(i), nctrl_pts(i));
                N[i]       = MatrixX<T>::Zero(1, nctrl_pts(i));

                mfa_data.OrigBasisFuns(i, param(i), span[i], N[i], 0);
            }

            // linear index of first control point
            ctrl_idx = 0;
            for (int j = 0; j < mfa_data.p.size(); j++)
                ctrl_idx += (span[j] - mfa_data.p(j) + ct(0, j)) * cs[j];
            size_t start_ctrl_idx = ctrl_idx;

            while (!vol_iter.done())
            {
                // always compute the point in the first dimension
                ctrl_pt = ctrl_pts.row(ctrl_idx);
                T w     = weights(ctrl_idx);

#ifdef WEIGH_ALL_DIMS                                       // weigh all dimensions
                temp[0] += (N[0])(0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0)) * ctrl_pt * w;
#else                                                       // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (temp[0])(j) += (N[0])(0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0)) * ctrl_pt(j);
                (temp[0])(last) += (N[0])(0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0)) * ctrl_pt(last) * w;
#endif

                temp_denom(0) += w * N[0](0, vol_iter.idx_dim(0) + span[0] - mfa_data.p(0));

                vol_iter.incr_iter();                                           // must call near bottom of loop, but before checking for done span below

                // for all dimensions except last, check if span is finished
                ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa_data.p.size(); k++)
                {
                    if (vol_iter.cur_iter() < vol_iter.tot_iters())
                        ctrl_idx += ct(vol_iter.cur_iter(), k) * cs[k];         // ctrl_idx for the next iteration
                    if (k < mfa_data.dom_dim - 1 && vol_iter.done(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        temp[k + 1]        += (N[k + 1])(0, vol_iter.prev_idx_dim(k + 1) + span[k + 1] - mfa_data.p(k + 1)) * temp[k];
                        temp_denom(k + 1)  += temp_denom(k) * N[k + 1](0, vol_iter.prev_idx_dim(k + 1) + span[k + 1] - mfa_data.p(k + 1));
                        temp_denom(k)       = 0.0;
                        temp[k]             = VectorX<T>::Zero(last + 1);
                    }
                }
            }

            T denom;                                        // rational denominator
            denom = temp_denom(mfa_data.p.size() - 1);

#ifdef WEIGH_ALL_DIMS                                       // weigh all dimensions
            out_pt = temp[mfa_data.p.size() - 1] / denom;
#else                                                       // weigh only range dimension
            out_pt   = temp[mfa_data.p.size() - 1];
            out_pt(last) /= denom;
#endif

        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // fastest version for multiple points, reuses saved basis functions
        // only values, no derivatives, because basis functions were not saved for derivatives
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt_saved_basis(
                const VectorXi&             ijk,        // ijk index of input domain point being decoded
                const VectorX<T>&           param,      // parameter value in each dim. of desired point
                VectorX<T>&                 out_pt,     // (output) point, allocated by caller
                DecodeInfo<T>&              di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                const TensorProduct<T>&     tensor)     // tensor product to use for decoding
        {
            int last = tensor.ctrl_pts.cols() - 1;

            di.Reset_saved_basis(mfa_data);

            // set up the volume iterator
            VectorXi npts = mfa_data.p + VectorXi::Ones(mfa_data.dom_dim);      // local support is p + 1 in each dim.
            VolIterator vol_iter(npts);                                         // for iterating in a flat loop over n dimensions

            // linear index of first control point
            di.ctrl_idx = 0;
            for (int j = 0; j < mfa_data.dom_dim; j++)
            {
                di.span[j]    = mfa_data.FindSpan(j, param(j), tensor);
                di.ctrl_idx += (di.span[j] - mfa_data.p(j) + ct(0, j)) * cs[j];
            }
            size_t start_ctrl_idx = di.ctrl_idx;

            while (!vol_iter.done())
            {
                // always compute the point in the first dimension
                di.ctrl_pt  = tensor.ctrl_pts.row(di.ctrl_idx);
                T w         = tensor.weights(di.ctrl_idx);

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
                di.temp[0] += (mfa_data.N[0])(ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt * w;
#else                                                                           // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (di.temp[0])(j) += (mfa_data.N[0])(ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt(j);
                (di.temp[0])(last) += (mfa_data.N[0])(ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt(last) * w;
#endif

                di.temp_denom(0) += w * mfa_data.N[0](ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0));

                vol_iter.incr_iter();                                           // must call near bottom of loop, but before checking for done span below

                // for all dimensions except last, check if span is finished
                di.ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa_data.dom_dim; k++)
                {
                    if (vol_iter.cur_iter() < vol_iter.tot_iters())
                        di.ctrl_idx += ct(vol_iter.cur_iter(), k) * cs[k];      // ctrl_idx for the next iteration
                    if (k < mfa_data.dom_dim - 1 && vol_iter.done(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        // use prev_idx_dim because iterator was already incremented above
                        di.temp[k + 1]        += (mfa_data.N[k + 1])(ijk(k + 1), vol_iter.prev_idx_dim(k + 1) + di.span[k + 1] - mfa_data.p(k + 1)) * di.temp[k];
                        di.temp_denom(k + 1)  += di.temp_denom(k) * mfa_data.N[k + 1](ijk(k + 1), vol_iter.prev_idx_dim(k + 1) + di.span[k + 1] - mfa_data.p(k + 1));
                        di.temp_denom(k)       = 0.0;
                        di.temp[k].setZero();
                    }
                }
            }

            T denom = di.temp_denom(mfa_data.dom_dim - 1);                      // rational denominator

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
            out_pt = di.temp[mfa_data.dom_dim - 1] / denom;
#else                                                                           // weigh only range dimension
            out_pt   = di.temp[mfa_data.dom_dim - 1];
            out_pt(last) /= denom;
#endif
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // fastest version for multiple points, reuses computed basis functions
        // only values, no derivatives, because basis functions were not saved for derivatives
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt_saved_basis_grid(
                const VectorXi&             ijk,        // ijk index of grid domain point being decoded
                const VectorX<T>&           param,      // parameter value in each dim. of desired point
                VectorX<T>&                 out_pt,     // (output) point, allocated by caller
                DecodeInfo<T>&              di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                const TensorProduct<T>&     tensor,     // tensor product to use for decoding
                vector<MatrixX<T>>&         NN )        // precomputed basis functions at grid
        {
            int last = tensor.ctrl_pts.cols() - 1;

            di.Reset_saved_basis(mfa_data);

            // set up the volume iterator
            VectorXi npts = mfa_data.p + VectorXi::Ones(mfa_data.dom_dim);      // local support is p + 1 in each dim.
            VolIterator vol_iter(npts);                                         // for iterating in a flat loop over n dimensions

            // linear index of first control point
            di.ctrl_idx = 0;
            for (int j = 0; j < mfa_data.dom_dim; j++)
            {
                di.span[j]    = mfa_data.FindSpan(j, param(j), tensor);
                di.ctrl_idx += (di.span[j] - mfa_data.p(j) + ct(0, j)) * cs[j];
            }
            size_t start_ctrl_idx = di.ctrl_idx;

            while (!vol_iter.done())
            {
                // always compute the point in the first dimension
                di.ctrl_pt  = tensor.ctrl_pts.row(di.ctrl_idx);
                T w         = tensor.weights(di.ctrl_idx);

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
                di.temp[0] += (NN[0])(ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt * w;
#else                                                                           // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (di.temp[0])(j) += (NN[0])(ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt(j);
                (di.temp[0])(last) += (NN[0])(ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt(last) * w;
#endif

                di.temp_denom(0) += w * NN[0](ijk(0), vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0));

                vol_iter.incr_iter();                                           // must call near bottom of loop, but before checking for done span below

                // for all dimensions except last, check if span is finished
                di.ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa_data.dom_dim; k++)
                {
                    if (vol_iter.cur_iter() < vol_iter.tot_iters())
                        di.ctrl_idx += ct(vol_iter.cur_iter(), k) * cs[k];      // ctrl_idx for the next iteration
                    if (k < mfa_data.dom_dim - 1 && vol_iter.done(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        // use prev_idx_dim because iterator was already incremented above
                        di.temp[k + 1]        += (NN[k + 1])(ijk(k + 1), vol_iter.prev_idx_dim(k + 1) + di.span[k + 1] - mfa_data.p(k + 1)) * di.temp[k];
                        di.temp_denom(k + 1)  += di.temp_denom(k) * NN[k + 1](ijk(k + 1), vol_iter.prev_idx_dim(k + 1) + di.span[k + 1] - mfa_data.p(k + 1));
                        di.temp_denom(k)       = 0.0;
                        di.temp[k].setZero();
                    }
                }
            }

            T denom = di.temp_denom(mfa_data.dom_dim - 1);                      // rational denominator

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
            out_pt = di.temp[mfa_data.dom_dim - 1] / denom;
#else                                                                           // weigh only range dimension
            out_pt   = di.temp[mfa_data.dom_dim - 1];
            out_pt(last) /= denom;
#endif
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // faster version for multiple points, reuses decode info, but recomputes basis functions
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt(
                const VectorX<T>&       param,      // parameter value in each dim. of desired point
                VectorX<T>&             out_pt,     // (output) point, allocated by caller
                DecodeInfo<T>&          di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                const TensorProduct<T>& tensor,     // tensor product to use for decoding
                const VectorXi&         derivs)     // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                    // pass size-0 vector if unused
        {
            int last = tensor.ctrl_pts.cols() - 1;
            if (derivs.size())                                                  // extra check for derivatives, won't slow down normal point evaluation
            {
                if (derivs.size() != mfa_data.dom_dim)
                {
                    fprintf(stderr, "Error: size of derivatives vector is not the same as the number of domain dimensions\n");
                    exit(0);
                }
                for (auto i = 0; i < mfa_data.dom_dim; i++)
                    if (derivs(i) > mfa_data.p(i))
                        fprintf(stderr, "Warning: In dimension %d, trying to take derivative %d of an MFA with degree %d will result in 0. This may not be what you want",
                                i, derivs(i), mfa_data.p(i));
            }

            di.Reset(mfa_data, derivs);

            // set up the volume iterator
            VectorXi npts = mfa_data.p + VectorXi::Ones(mfa_data.dom_dim);      // local support is p + 1 in each dim.
            VolIterator vol_iter(npts);                                         // for iterating in a flat loop over n dimensions

            // basis funs
            for (size_t i = 0; i < mfa_data.dom_dim; i++)                       // for all dims
            {
                di.span[i]    = mfa_data.FindSpan(i, param(i), tensor);

                if (derivs.size() && derivs(i))
                {
#ifndef MFA_TMESH   // original version for one tensor product
                    mfa_data.DerBasisFuns(i, param(i), di.span[i], derivs(i), di.ders[i]);
                    di.N[i].row(0) = di.ders[i].row(derivs(i));
#endif
                }
                else
                {
#ifndef MFA_TMESH   // original version for one tensor product
                    mfa_data.OrigBasisFuns(i, param(i), di.span[i], di.N[i], 0);
#else               // tmesh version
                    mfa_data.BasisFuns(i, param(i), di.span[i], di.N[i], 0);
#endif
                }
            }

            // linear index of first control point
            di.ctrl_idx = 0;
            for (int j = 0; j < mfa_data.dom_dim; j++)
                di.ctrl_idx += (di.span[j] - mfa_data.p(j) + ct(0, j)) * cs[j];
            size_t start_ctrl_idx = di.ctrl_idx;

            while (!vol_iter.done())
            {
                // always compute the point in the first dimension
                di.ctrl_pt  = tensor.ctrl_pts.row(di.ctrl_idx);
                T w         = tensor.weights(di.ctrl_idx);

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
                di.temp[0] += (di.N[0])(0, vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt * w;
#else                                                                           // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (di.temp[0])(j) += (di.N[0])(0, vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt(j);
                (di.temp[0])(last) += (di.N[0])(0, vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0)) * di.ctrl_pt(last) * w;
#endif

                di.temp_denom(0) += w * di.N[0](0, vol_iter.idx_dim(0) + di.span[0] - mfa_data.p(0));

                vol_iter.incr_iter();                                           // must call near bottom of loop, but before checking for done span below

                // for all dimensions except last, check if span is finished
                di.ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa_data.dom_dim; k++)
                {
                    if (vol_iter.cur_iter() < vol_iter.tot_iters())
                        di.ctrl_idx += ct(vol_iter.cur_iter(), k) * cs[k];      // ctrl_idx for the next iteration
                    if (k < mfa_data.dom_dim - 1 && vol_iter.done(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        // use prev_idx_dim because iterator was already incremented above
                        di.temp[k + 1]        += (di.N[k + 1])(0, vol_iter.prev_idx_dim(k + 1) + di.span[k + 1] - mfa_data.p(k + 1)) * di.temp[k];
                        di.temp_denom(k + 1)  += di.temp_denom(k) * di.N[k + 1](0, vol_iter.prev_idx_dim(k + 1) + di.span[k + 1] - mfa_data.p(k + 1));
                        di.temp_denom(k)       = 0.0;
                        di.temp[k].setZero();
                    }
                }
            }

            T denom;                                                            // rational denominator
            if (derivs.size() && derivs.sum())
                denom = 1.0;                                                    // TODO: weights for derivatives not implemented yet
            else
                denom = di.temp_denom(mfa_data.dom_dim - 1);

#ifdef WEIGH_ALL_DIMS                                                           // weigh all dimensions
            out_pt = di.temp[mfa_data.dom_dim - 1] / denom;
#else                                                                           // weigh only range dimension
            out_pt   = di.temp[mfa_data.dom_dim - 1];
            out_pt(last) /= denom;
#endif
        }


        void FastGrad(
            const VectorX<T>&           param,
            FastDecodeInfo<T>&          di,
            const TensorProduct<T>&     tensor,
            VectorX<T>&                 out_grad,
            T*                          out_val = nullptr)
        {
#ifdef MFA_TMESH
cerr << "ERROR: Cannot use FastGrad with TMesh" << endl;
exit(1);
#endif
#ifndef MFA_NO_WEIGHTS
cerr << "ERROR: Must define MFA_NO_WEIGHTS to use FastGrad" << endl;
exit(1);
#endif      

            assert(di.D[0].size() == 2);   // ensures D has been resized to hold 1st derivs
            assert(di.M != nullptr);

            // Compute the point value of the B-spline if out_val is not NULL
            int end_d = -1;
            if (out_val == nullptr)
                end_d = dom_dim;
            else
                end_d = dom_dim + 1;

            // Compute spans, basis functions, and derivatives of basis functions for the given parameters
            // This small loop accounts for ~40% of the total time for this method (measured 11/16/21 for 3d, p=4, ctrlpts=30)
            for (int i = 0; i < dom_dim; i++)
            {
                di.span[i] = mfa_data.FindSpan(i, param(i));
                mfa_data.FastBasisFunsDers(i, param(i), di.span[i], 1, di.D[i], di.bfi);
            }

            // The remainder of this method computes the usual sum for decoding points:
            //
            // sum_i sum_j ... sum_l N_i()*N_j()*...*N_l() * P_ij...l 
            //
            // except, for each directional derivative we multiply by the derivative of 
            // the corresponding basis function instead.
            //
            // This is best computed as a series of "n-mode tensor-vector products":
            // sum_i N_i * (sum_j N_j * (.... * (sum_l N_l*P_ij...l)))
            // (see "Tensor Decompositions and Applications", Kolda and Bader, chap 2.5)
            // 
            // that is, we compute the inner sum (with the control points) first, 
            // and then multiply that with the next most-inner sum, working our way out.
            // This requires less computational complexity than a VolIterator-style sum.

            // Description of the M[][][] notation:
            //     As we loop through the different domain dimensions, sometimes we need 
            //     to multiply by basis functions and sometimes we need to multiply by 
            //     products of basis functions. In particular, if we are computing the 
            //     deriv in the i^th directions, then we must replace basis functions
            //     in the i^th direction with derivs in the i^th direction.
            //
            //     We don't want any if-else logic buried deep in a nested loop, so we 
            //     create a structure, M,which aliases the vectors of basis functions 
            //     and derivatives of basis functions.
            // 
            //     In particular, M[d][k] points to the vector of basis functions in the
            //     k^th direction, EXCEPT when d=k. When d=k, M[d][k] points to the 
            //     vector of derivatives of basis functions in the k^th direction. This 
            //     allows us to write a loop over all derivative directions that doesn't
            //     need to switch  based on the special case when d=k.
            //     
            //     M is initialized by a call to FastDecodeInfo::ResizeDers(), which MUST
            //     be called ahead of time. This method allocates/frees memory and 
            //     should not be called repeatedly. In practice, it should  only be 
            //     necessary to call ResizeDers() once, probably right after construction
            //     of the FastDecodeInfo object.
            //
            //     The pointers in M alias the matrices di.D[0], di.D[1], .... Therefore,
            //     it is only necessary to fill these D matrices and the data can then 
            //     be accessed via M.

            // compute linear index of first control point
            int start_ctrl_idx = 0;
            for (int j = 0; j < dom_dim; j++)
                start_ctrl_idx += (di.span[j] - mfa_data.p(j)) * cs[j];

            // Compute the 0-mode vector product
            // This is the only time we need to access the control points
            for (int m = 0, id = 0; m < tot_iters; m += q0, id++)
            {
                di.ctrl_idx = start_ctrl_idx + jumps(m);

                // Separate 1st iteration to avoid zero-initialization
                di.td[0][0][id] = di.M[0][0][0] * tensor.ctrl_pts(di.ctrl_idx);
                di.t[0][id] = di.M[1][0][0] * tensor.ctrl_pts(di.ctrl_idx);
                for (int a = 1; a < q0; a++)
                {
                    // For this first loop, there are only two cases: multiply control 
                    // points by the basis functions, or multiply control points by 
                    // derivative of basis functions. We save time by only computing 
                    // each case once, and then copying the result as needed, below.
                    di.td[0][0][id] += di.M[0][0][a] * tensor.ctrl_pts(di.ctrl_idx + a);    // der basis fun * ctl pts
                    di.t[0][id] += di.M[1][0][a] * tensor.ctrl_pts(di.ctrl_idx + a);        // basis fun * ctl pts
                }
            }
            for (int d = 1; d < end_d; d++) // In this special case, the values for d >= 1 are the same 
            {
                for (int id = 0; id < di.td[d][0].size(); id++)
                {
                    di.td[d][0][id] = di.t[0][id];
                }
            }

            // For each derivative, d, compute the remaining k-mode vector products.
            int qcur = 0, tsz = 0;
            for (int d = 0; d < end_d; d++) // for each derivative
            {
                for (int k = 1; k < dom_dim; k++)   // for each direction to perform a k-mode tensor-vector product
                {
                    qcur = q[k];
                    tsz = di.td[d][k-1].size();

                    // Perform the k-mode vector product
                    for (int m = 0, id = 0; m < tsz; m += qcur, id++)
                    {
                        di.td[d][k][id] = di.M[d][k][0] * di.td[d][k-1][m];
                        for (int l = 1; l < qcur; l++)
                        {
                            di.td[d][k][id] += di.M[d][k][l] * di.td[d][k-1][m + l];
                        }
                    }
                }
            } 

            for (int d = 0; d < dom_dim; d++)
                out_grad(d) = di.td[d][dom_dim - 1][0];

            if (out_val != nullptr)
                *out_val = di.td[dom_dim][dom_dim - 1][0];
        }

        // Fast implementation of VolPt for simple MFA models
        // Requirements:
        //   * Model does not use weights (must define MFA_NO_WEIGHTS)
        //   * Science variable must be one-dimensional
        //   * Cannot compute derivatives
        //   * Does not support TMesh
        void FastVolPt(
            const VectorX<T>&           param,      // parameter value in each dim. of desired point
                VectorX<T>&             out_pt,     // (output) point, allocated by caller
                FastDecodeInfo<T>&      di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                const TensorProduct<T>& tensor) const    // tensor product to use for decoding
        {
#ifdef MFA_TMESH
cerr << "ERROR: Cannot use FastVolPt with TMesh" << endl;
exit(1);
#endif
#ifndef MFA_NO_WEIGHTS
cerr << "ERROR: Must define MFA_NO_WEIGHTS to use FastVolPt" << endl;
exit(1);
#endif
            // compute spans and basis functions for the given parameters
            for (int i = 0; i < dom_dim; i++)
            {
                di.span[i] = mfa_data.FindSpan(i, param(i));

                mfa_data.FastBasisFuns(i, param(i), di.span[i], di.N[i], di.bfi);
            }

            // compute linear index of first control point
            int start_ctrl_idx = 0;
            for (int j = 0; j < mfa_data.dom_dim; j++)
                start_ctrl_idx += (di.span[j] - mfa_data.p(j)) * cs[j];


            // * The remaining loops perform the sums and products of basis functions across different
            //   dimensions. This loop looks different from the old VolPt loop in order to remove the
            //   step to check if a control point is at the "end" of some dimension. Instead, we compute
            //   a series of temporary sums, which are stored in di.t[i] (i = current dimension).
            // * We separate out the first dimension because this is the only place where
            //   control points are accessed. 
            // * This setup requires more temporary vectors (the largest of which is of size q^{d-1}), but
            //   the time spent accumulating basis functions is reduced by about 10-20%

            // First domain dimension, we multiply basis functions with control points
            for (int m = 0, id = 0; m < tot_iters; m += q0, id++)
            {
                di.ctrl_idx = start_ctrl_idx + jumps(m);

                di.t[0][id] = di.N[0][0] * tensor.ctrl_pts(di.ctrl_idx);
                for (int a = 1; a < q0; a++)
                {
                    di.t[0][id] += di.N[0][a] * tensor.ctrl_pts(di.ctrl_idx + a);
                }
            }

            // For all subsequent dimensions, we multiply basis functions with temporary sums
            int qcur = 0, tsz = 0;
            for (int k = 1; k < mfa_data.dom_dim; k++)
            {
                qcur = q[k];
                tsz = di.t[k-1].size();
                for (int m = 0, id = 0; m < tsz; m += qcur, id++)
                {
                    di.t[k][id] = di.N[k][0] * di.t[k-1][m];
                    for (int l = 1; l < qcur; l++)
                    {
                        di.t[k][id] += di.N[k][l] * di.t[k-1][m + l];
                    }
                }
            }

            out_pt(0) = di.t[mfa_data.dom_dim - 1][0];
        }

        // compute a point from a NURBS curve at a given parameter value
        // this version takes a temporary set of control points for one curve only rather than
        // reading full n-d set of control points from the mfa
        // algorithm 4.1, Piegl & Tiller (P&T) p.124
        void CurvePt(
                int                             cur_dim,        // current dimension
                T                               param,          // parameter value of desired point
                const MatrixX<T>&               temp_ctrl,      // temporary control points
                const VectorX<T>&               temp_weights,   // weights associate with temporary control points
                const TensorProduct<T>&         tensor,         // current tensor product
                VectorX<T>&                     out_pt)         // (output) point
        {
            int span   = mfa_data.FindSpan(cur_dim, param, tensor);
            MatrixX<T> N = MatrixX<T>::Zero(1, temp_ctrl.rows());// basis coefficients

#ifndef MFA_TMESH                                               // original version for one tensor product

            mfa_data.OrigBasisFuns(cur_dim, param, span, N, 0);

#else                                                           // tmesh version

            mfa_data.BasisFuns(cur_dim, param, span, N, 0);

#endif
            out_pt = VectorX<T>::Zero(temp_ctrl.cols());        // initializes and resizes

            for (int j = 0; j <= mfa_data.p(cur_dim); j++)
                out_pt += N(0, j + span - mfa_data.p(cur_dim)) *
                    temp_ctrl.row(span - mfa_data.p(cur_dim) + j) *
                    temp_weights(span - mfa_data.p(cur_dim) + j);

            // compute the denominator of the rational curve point and divide by it
            // sum of element-wise multiplication requires transpose so that both arrays are same shape
            // (rows in this case), otherwise eigen cannot multiply them
            T denom = (N.row(0).cwiseProduct(temp_weights.transpose())).sum();
            out_pt /= denom;
        }

    };
}

#endif
