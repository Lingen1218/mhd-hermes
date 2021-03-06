#include <stdio.h>
#include <stdexcept>

#include "dummy_solver.h"
#include "hermes2d.h"
#include "_hermes2d_api.h"

// The time-dependent laminar incompressible Navier-Stokes equations are
// discretized in time via the implicit Euler method. The convective term
// is linearized simply by replacing the velocity in front of the nabla
// operator with the velocity from last time step. Velocity is approximated
// using continuous elements, and pressure by means or discontinuous (L2)
// elements. This makes the velocity discretely divergence-free, meaning
// that the integral of div(v) over every element is zero. The problem
// has a steady symmetric solution which is unstable. Note that after some
// time (around t = 100), numerical errors induce oscillations. The
// approximation becomes unsteady and thus diverges from the exact solution.
// Interestingly, this happens even with a completely symmetric mesh.
//
// PDE: incompressible Navier-Stokes equations in the form
// \partial v / \partial t - \Delta v / Re + (v \cdot \nabla) v + \nabla p = 0,
// div v = 0
//
// BC: u_1 is a time-dependent constant and u_2 = 0 on Gamma_4 (inlet)
//     u_1 = u_2 = 0 on Gamma_1 (bottom), Gamma_3 (top) and Gamma_5 (obstacle)
//     "do nothing" on Gamma_2 (outlet)
//
// TODO: Implement Crank-Nicolson so that comparisons with implicit Euler can be made
//
// The following parameters can be played with:

const double RE = 1000.0;            // Reynolds number
const double VEL_INLET = 1.0;        // inlet velocity (reached after STARTUP_TIME)
const double STARTUP_TIME = 1.0;     // during this time, inlet velocity increases gradually
                                     // from 0 to VEL_INLET, then it stays constant
const double TAU = 0.5;              // time step
const double FINAL_TIME = 3000.0;    // length of time interval
const int P_INIT_VEL = 2;            // polynomial degree for velocity components
const int P_INIT_PRESSURE = 1;       // polynomial degree for pressure
                                     // Note: P_INIT_VEL should always be greater than
                                     // P_INIT_PRESSURE because of the inf-sup condition
const double H = 10;                // domain height (necessary to define the parabolic
                                     // velocity profile at inlet)

//  boundary markers
int marker_bottom = 1;
int marker_right  = 2;
int marker_top = 3;
int marker_left = 4;
int marker_obstacle = 5;

// global time variable
double TIME = 0;

// definition of boundary conditions
int xvel_bc_type(int marker) {
  if (marker == marker_right) return BC_NONE;
  else return BC_ESSENTIAL;
}

int yvel_bc_type(int marker) {
  if (marker == marker_right) return BC_NONE;
  else return BC_ESSENTIAL;
}

int press_bc_type(int marker)
  { return BC_NONE; }

scalar xvel_bc_value(int marker, double x, double y) {
  if (marker == marker_left) {
    // time-dependent inlet velocity
    //double val_y = VEL_INLET; //constant profile
    double val_y = VEL_INLET * y*(H-y) / (H/2.)/(H/2.); //parabolic profile with peak VEL_INLET at y = H/2
    if (TIME <= STARTUP_TIME) return val_y * TIME/STARTUP_TIME;
    else return val_y;
  }
  else return 0;
}

// velocities from the previous time step
Solution xprev, yprev;

scalar bilinear_form_sym_0_0_1_1(RealFunction* fu, RealFunction* fv, RefMap* ru, RefMap* rv)
  { return int_grad_u_grad_v(fu, fv, ru, rv) / RE +
           int_u_v(fu, fv, ru, rv) / TAU; }

scalar bilinear_form_unsym_0_0_1_1(RealFunction* fu, RealFunction* fv, RefMap* ru, RefMap* rv)
  { return int_w_nabla_u_v(&xprev, &yprev, fu, fv, ru, rv); }

scalar bilinear_form_unsym_0_2(RealFunction* fp, RealFunction* fv, RefMap* rp, RefMap* rv)
  { return -int_u_dvdx(fp, fv, rp, rv); }

scalar bilinear_form_unsym_1_2(RealFunction* fp, RealFunction* fv, RefMap* rp, RefMap* rv)
  { return -int_u_dvdy(fp, fv, rp, rv); }

scalar linear_form_0(RealFunction* fv, RefMap* rv)
  { return int_u_v(&xprev, fv, xprev.get_refmap(), rv) / TAU; }

scalar linear_form_1(RealFunction* fv, RefMap* rv)
  { return int_u_v(&yprev, fv, yprev.get_refmap(), rv) / TAU; }


int main(int argc, char* argv[])
{
  // Initialize Python
  Py_Initialize();
  PySys_SetArgv(argc, argv);
  if (import_hermes2d___hermes2d())
      throw std::runtime_error("hermes2d failed to import.");
  cmd("import utils");

  // load the mesh file
  Mesh mesh;
  mesh.load("domain-quad.mesh"); // unstructured triangular mesh available in domain-tri.mesh

  // a-priori mesh refinements
  mesh.refine_all_elements();
  mesh.refine_towards_boundary(marker_obstacle, 4, false);
  //mesh.refine_towards_boundary(marker_bottom, 4);
  //mesh.refine_towards_boundary(marker_top, 4);
  // plot the mesh:
  //insert_object("mesh", Mesh_from_C(&mesh));
  //cmd("mesh.plot(lib='mpl', method='orders')");

  // display the mesh
  //MeshView mview("Navier-Stokes Example - Mesh", 100, 100, 1100, 400);
  //mview.show(&mesh);
  //mview.wait_for_keypress();

  // initialize the shapesets and the cache
  H1ShapesetBeuchler shapeset_h1;
  PrecalcShapeset pss_h1(&shapeset_h1);
  L2Shapeset shapeset_l2;
  PrecalcShapeset pss_l2(&shapeset_l2);

  // H1 spaces for velocities and L2 for pressure
  H1Space xvel(&mesh, &shapeset_h1);
  H1Space yvel(&mesh, &shapeset_h1);
  L2Space press(&mesh, &shapeset_l2);

  // initialize boundary conditions
  xvel.set_bc_types(xvel_bc_type);
  xvel.set_bc_values(xvel_bc_value);
  yvel.set_bc_types(yvel_bc_type);
  press.set_bc_types(press_bc_type);

  // set velocity and pressure polynomial degrees
  xvel.set_uniform_order(P_INIT_VEL);
  yvel.set_uniform_order(P_INIT_VEL);
  press.set_uniform_order(P_INIT_PRESSURE);

  // assign degrees of freedom
  int ndofs = 0;
  ndofs += xvel.assign_dofs(ndofs);
  ndofs += yvel.assign_dofs(ndofs);
  ndofs += press.assign_dofs(ndofs);

  // initial BC: xprev and yprev are zero
  xprev.set_zero(&mesh);
  yprev.set_zero(&mesh);

  // set up weak formulation
  WeakForm wf(3);
  wf.add_biform(0, 0, bilinear_form_sym_0_0_1_1, SYM);
  wf.add_biform(0, 0, bilinear_form_unsym_0_0_1_1, UNSYM, ANY, 2, &xprev, &yprev);
  wf.add_biform(1, 1, bilinear_form_sym_0_0_1_1, SYM);
  wf.add_biform(1, 1, bilinear_form_unsym_0_0_1_1, UNSYM, ANY, 2, &xprev, &yprev);
  wf.add_biform(0, 2, bilinear_form_unsym_0_2, ANTISYM);
  wf.add_biform(1, 2, bilinear_form_unsym_1_2, ANTISYM);
  wf.add_liform(0, linear_form_0, ANY, 1, &xprev);
  wf.add_liform(1, linear_form_1, ANY, 1, &yprev);

  // visualization
  //VectorView vview("velocity [m/s]", 0, 0, 1500, 470);
  //ScalarView pview("pressure [Pa]", 0, 530, 1500, 470);
  //vview.set_min_max_range(0, 1.6);
  //pview.show_mesh(false);
  // fixing scale width (for nicer videos). Note: creation of videos is
  // discussed in a separate example
  //vview.fix_scale_width(5);
  //pview.fix_scale_width(5);

  // set up the linear system
  DummySolver umfpack;
  LinSystem sys(&wf, &umfpack);
  sys.set_spaces(3, &xvel, &yvel, &press);
  sys.set_pss(3, &pss_h1, &pss_h1, &pss_l2);


  cmd("from hermes2d import Linearizer, Vectorizer");
  cmd("from scipy.sparse import csc_matrix");
  cmd("from scipy.sparse.linalg.dsolve import spsolve");
  cmd("from scipy.sparse.linalg import cg");

  // main loop
  char title[100];
  int num_time_steps = FINAL_TIME / TAU;
  for (int i = 1; i <= num_time_steps; i++)
  {
    TIME += TAU;

    info("\n---- Time step %d, time = %g -----------------------------------", i, TIME);

    // this is needed to update the time-dependent boundary conditions
    ndofs = 0;
    ndofs += xvel.assign_dofs(ndofs);
    ndofs += yvel.assign_dofs(ndofs);
    ndofs += press.assign_dofs(ndofs);

    // assemble and solve
    Solution xsln, ysln, psln;
    psln.set_zero(&mesh);
    sys.assemble();
    //sys.solve(3, &xsln, &ysln, &psln);

    int *Ap, *Ai, n, nnz;
    scalar *Ax;
    sys.get_matrix(Ap, Ai, Ax, n);
    nnz = Ap[n];
    scalar *rhs;
    sys.get_rhs(rhs, n);
    insert_int_array("Ap", Ap, n+1);
    insert_int_array("Ai", Ai, nnz);
    insert_double_array("Ax", Ax, nnz);
    insert_double_array("rhs", rhs, n);
    cmd("A = csc_matrix((Ax, Ai, Ap))");
    cmd("x = spsolve(A, rhs)");
    double *X;
    array_double_numpy2c_inplace(get_symbol("x"), &X, &n);
    xsln.set_fe_solution(sys.get_space(0), sys.get_pss(0), X);
    ysln.set_fe_solution(sys.get_space(1), sys.get_pss(1), X);
    psln.set_fe_solution(sys.get_space(2), sys.get_pss(2), X);
    insert_object("xsln", Solution_from_C(&xsln));
    insert_object("ysln", Solution_from_C(&ysln));
    insert_object("psln", Solution_from_C(&psln));
    cmd("l = Linearizer()");
    cmd("l.process_solution(psln)");
    cmd("vert = l.get_vertices()");
    cmd("triangles = l.get_triangles()");
    cmd("utils.plot(vert, triangles)");

    cmd("v = Vectorizer()");
    cmd("v.process_solution(xsln, ysln)");
    cmd("v_vert = v.get_vertices()");
    cmd("v_triangles = v.get_triangles()");
    cmd("utils.plot_vec(v_vert, v_triangles)");

    //cmd("import IPython; IPython.ipapi.set_trace()");

    // visualization
    sprintf(title, "Velocity, time %g", TIME);
    //vview.set_title(title);
    //vview.show(&xprev, &yprev, EPS_LOW);
    sprintf(title, "Pressure, time %g", TIME);
    //pview.set_title(title);
    //pview.show(&psln);

    xprev = xsln;
    yprev = ysln;
  }

  //View::wait();
}
