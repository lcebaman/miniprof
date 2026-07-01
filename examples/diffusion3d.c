/*
 * diffusion3d.c — 3D multiphysics heat equation in heterogeneous media
 *                 with 2D Cartesian MPI domain decomposition
 *
 * Models coupled heat transport in a porous geothermal reservoir or
 * semiconductor package: anisotropic conduction in a heterogeneous
 * solid, combined with radiative cooling, pressure-driven Darcy
 * advection, viscous shear heating, and latent-heat phase change.
 * Each physical process is a separate function — deliberately designed
 * to present distinct, clearly attributable hot spots to the profiler.
 *
 * Governing equation (per-cell):
 *
 *   ρcp ∂T/∂t = ∇·(K∇T)          [anisotropic diffusion]
 *             + σε(T_env⁴ − T⁴)   [Stefan-Boltzmann radiation]
 *             − ρcp u·∇T           [Darcy-flow advection]
 *             + η|∇u|²             [viscous shear heating]
 *             − L ∂φ/∂t            [latent heat from phase change]
 *             + Q(x,y,z,t)         [prescribed volumetric source]
 *
 * where φ ∈ [0,1] is the liquid fraction (0=solid, 1=fully melted).
 *
 * Each right-hand-side term is a separate profiling hot spot:
 *
 *   compute_diffusion()          — 7-point stencil, face-averaged K
 *   compute_radiation()          — Stefan-Boltzmann T^4 cooling per cell
 *   compute_darcy_advection()    — Darcy-velocity advection, upwind scheme
 *   compute_viscous_dissipation()— shear heating from velocity gradients
 *   compute_phase_change()       — enthalpy-method latent heat update
 *   update_source()              — time-modulated Gaussian ring source
 *   halo_exchange_x/y()          — MPI face exchange (separately timed)
 *   apply_bc()                   — Dirichlet BCs on Z planes
 *   compute_diagnostics()        — global reductions via MPI_Allreduce
 *
 * Physical parameters chosen for geological-scale (1m³ rock) but the
 * code is dimensionless-friendly — change K0, RHO_CP, etc. to rescale.
 *
 * Build:
 *   make example-3d                        (default 256×256×128, 200 steps)
 *   make example-3d NX=128 NY=128 NZ=64   (smaller for quick testing)
 *   make example-3d-trace                  (instrumented binary)
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── compile-time grid and step parameters ───────────────────────────────── */
#ifndef NX_GLOBAL
#define NX_GLOBAL    256
#endif
#ifndef NY_GLOBAL
#define NY_GLOBAL    256
#endif
#ifndef NZ_GLOBAL
#define NZ_GLOBAL    128
#endif
#ifndef NSTEPS
#define NSTEPS       200
#endif
#ifndef DIAG_INTERVAL
#define DIAG_INTERVAL 20
#endif

/* ── physical constants ──────────────────────────────────────────────────── */
#define LX      1.0       /* domain length X [m]                             */
#define LY      1.0       /* domain length Y [m]                             */
#define LZ      0.5       /* domain length Z [m]                             */

/* diffusion */
#define K0      1.0e-4    /* background thermal conductivity [W/m·K]         */
#define K_ANOM  10.0      /* conductivity anomaly multiplier (heterogeneity)  */
#define RHO_CP  2.0e6     /* volumetric heat capacity ρcₚ [J/m³·K]           */

/* radiation */
#define SIGMA_SB 5.67e-8  /* Stefan-Boltzmann constant [W/m²·K⁴]            */
#define T_ENV   280.0     /* ambient/environment temperature [K]              */
#define EPS0    0.9       /* base emissivity (dimensionless)                  */

/* Darcy advection */
#define PERM    1.0e-13   /* permeability [m²] (typical sandstone)           */
#define VISC    1.0e-3    /* dynamic viscosity [Pa·s] (water at 20°C)        */
#define DP_DZ  -1.0e4     /* background pressure gradient ∂P/∂z [Pa/m]      */

/* viscous dissipation */
#define ETA_VIS 1.0e-3    /* effective viscosity for shear heating [Pa·s]    */
#define U_CHAR  1.0e-5    /* characteristic Darcy velocity [m/s]             */

/* phase change */
#define T_MELT  350.0     /* melting temperature [K]                         */
#define T_RANGE   5.0     /* mushy-zone half-width [K]                       */
#define LATENT  3.34e5    /* latent heat of fusion L [J/kg]                  */
#define RHO     2500.0    /* density [kg/m³]                                  */
#define STEFAN  (RHO * LATENT / RHO_CP) /* Stefan number * ΔT [K]           */

/* source */
#define Q_PEAK  2.0e3     /* peak volumetric source [W/m³]                   */

/* ── data structures ─────────────────────────────────────────────────────── */
typedef struct {
    MPI_Comm cart_comm;
    int      dims[2];
    int      coords[2];
    int      rank_xm, rank_xp;
    int      rank_ym, rank_yp;
    MPI_Datatype yz_face;
    MPI_Datatype xz_face;
} Topo;

typedef struct {
    int    nx, ny, nz;    /* interior points on this rank            */
    int    ngx, ngy;      /* with ghosts: nx+2, ny+2                 */
    double dx, dy, dz;
    double dt;
} Grid;

/* Row-major 3D index: ix ∈ [0,ngx), iy ∈ [0,ngy), iz ∈ [0,nz) */
#define IDX(g, ix, iy, iz) \
    ((size_t)(ix) * (g)->ngy * (g)->nz + (size_t)(iy) * (g)->nz + (iz))

/* ── prototypes ──────────────────────────────────────────────────────────── */
static void setup_topology(Topo *t, int nranks, Grid *g);
static void setup_grid(Grid *g, const Topo *t, int nx_gl, int ny_gl);

static void init_fields(const Grid *g, const Topo *t,
                        double *T, double *phi,
                        double *Kx, double *Ky, double *Kz,
                        double *eps, double *Px, double *Py, double *Pz,
                        double *ux, double *uy, double *uz,
                        double *Q,
                        double dx_g, double dy_g);

static void halo_exchange_x(double *T, const Grid *g, const Topo *t);
static void halo_exchange_y(double *T, const Grid *g, const Topo *t);

static void compute_diffusion(const double *T,
                               const double *Kx, const double *Ky,
                               const double *Kz,
                               double *dT, const Grid *g);

static void compute_radiation(const double *T, const double *eps,
                               double *dT, const Grid *g);

static void compute_darcy_advection(const double *T,
                                     const double *ux,
                                     const double *uy,
                                     const double *uz,
                                     double *dT, const Grid *g);

static void compute_viscous_dissipation(const double *ux,
                                         const double *uy,
                                         const double *uz,
                                         double *dT, const Grid *g);

static void compute_phase_change(double *T, double *phi,
                                  double *dT, const Grid *g, double dt);

static void update_source(double *dT, const double *Q,
                           const Grid *g, double pulse);

static void apply_bc(double *T, double *phi, const Grid *g);

static void advance(double *T, const double *dT, const Grid *g, double dt);

static void compute_diagnostics(const double *T, const double *phi,
                                  const Grid *g, const Topo *t,
                                  int step, double time);

static void write_slice(const double *T, const Grid *g, const Topo *t,
                         int step);

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int world_rank, world_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    Topo topo; Grid g;
    setup_topology(&topo, world_size, &g);
    setup_grid(&g, &topo, NX_GLOBAL, NY_GLOBAL);

    size_t ncells = (size_t)g.ngx * g.ngy * g.nz;

    double *T   = calloc(ncells, sizeof(double));   /* temperature [K]       */
    double *phi = calloc(ncells, sizeof(double));   /* liquid fraction [0,1] */
    double *dT  = calloc(ncells, sizeof(double));   /* RHS accumulator [K/s] */
    double *Kx  = calloc(ncells, sizeof(double));   /* x-conductivity        */
    double *Ky  = calloc(ncells, sizeof(double));   /* y-conductivity        */
    double *Kz  = calloc(ncells, sizeof(double));   /* z-conductivity        */
    double *eps = calloc(ncells, sizeof(double));   /* emissivity            */
    double *Px  = calloc(ncells, sizeof(double));   /* pressure gradient ∂P/∂x */
    double *Py  = calloc(ncells, sizeof(double));   /* pressure gradient ∂P/∂y */
    double *Pz  = calloc(ncells, sizeof(double));   /* pressure gradient ∂P/∂z */
    double *ux  = calloc(ncells, sizeof(double));   /* Darcy velocity x [m/s]  */
    double *uy  = calloc(ncells, sizeof(double));   /* Darcy velocity y [m/s]  */
    double *uz  = calloc(ncells, sizeof(double));   /* Darcy velocity z [m/s]  */
    double *Q   = calloc(ncells, sizeof(double));   /* heat source [W/m³]    */

    if (!T || !phi || !dT || !Kx || !Ky || !Kz || !eps ||
        !Px || !Py || !Pz || !ux || !uy || !uz || !Q) {
        fprintf(stderr, "rank %d: allocation failed\n", world_rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    double dx_g = LX / NX_GLOBAL;
    double dy_g = LY / NY_GLOBAL;
    init_fields(&g, &topo, T, phi, Kx, Ky, Kz, eps, Px, Py, Pz,
                ux, uy, uz, Q, dx_g, dy_g);
    apply_bc(T, phi, &g);

    if (world_rank == 0) {
        printf("diffusion3d: %dx%dx%d global grid, %d ranks (%dx%d Cartesian)\n",
               NX_GLOBAL, NY_GLOBAL, NZ_GLOBAL,
               world_size, topo.dims[0], topo.dims[1]);
        printf("  local grid: %dx%dx%d interior (+2 ghosts in X,Y)\n",
               g.nx, g.ny, g.nz);
        printf("  dt=%.3e s  dx=%.3e m  %d steps\n", g.dt, g.dx, NSTEPS);
        printf("  Physics: diffusion + radiation + Darcy advection"
               " + viscous heating + phase change\n");
    }

    /* ── time integration ──────────────────────────────────────────────── */
    double t = 0.0;
    for (int step = 0; step < NSTEPS; step++) {

        /* halo exchange (ghost cells needed by stencil and upwind scheme) */
        halo_exchange_x(T, &g, &topo);
        halo_exchange_y(T, &g, &topo);

        /* accumulate all RHS contributions into dT [K/s] */
        memset(dT, 0, ncells * sizeof(double));

        compute_diffusion(T, Kx, Ky, Kz, dT, &g);           /* ∇·(K∇T)/ρcₚ */
        compute_radiation(T, eps, dT, &g);                    /* σε(T_env⁴-T⁴)/ρcₚ */
        compute_darcy_advection(T, ux, uy, uz, dT, &g);      /* -u·∇T */
        compute_viscous_dissipation(ux, uy, uz, dT, &g);     /* η|∇u|²/ρcₚ */

        double pulse = exp(-0.5 * pow((t - 0.05) / 0.02, 2));
        update_source(dT, Q, &g, pulse);                      /* Q(x,y,z,t) */

        /* explicit Euler step: T += dt * dT */
        advance(T, dT, &g, g.dt);

        /* phase change correction: applied after advance so it sees the
           tentative T_new; modifies T and phi in-place using the
           sequential enthalpy method (Voller & Prakash 1987).
           This preserves total enthalpy H = rho_cp*T + rho*L*phi. */
        compute_phase_change(T, phi, dT, &g, g.dt);

        apply_bc(T, phi, &g);
        t += g.dt;

        if ((step + 1) % DIAG_INTERVAL == 0 || step == 0)
            compute_diagnostics(T, phi, &g, &topo, step + 1, t);
    }

    write_slice(T, &g, &topo, NSTEPS);

    if (world_rank == 0)
        printf("diffusion3d: %d steps complete, t=%.4f s\n", NSTEPS, t);

    free(T); free(phi); free(dT);
    free(Kx); free(Ky); free(Kz); free(eps);
    free(Px); free(Py); free(Pz); free(ux); free(uy); free(uz); free(Q);
    MPI_Type_free(&topo.yz_face);
    MPI_Type_free(&topo.xz_face);
    MPI_Comm_free(&topo.cart_comm);
    MPI_Finalize();
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TOPOLOGY AND GRID SETUP
 * ═══════════════════════════════════════════════════════════════════════════ */

static void setup_topology(Topo *topo, int nranks, Grid *g)
{
    (void)g;
    memset(topo, 0, sizeof(*topo));
    topo->dims[0] = topo->dims[1] = 0;
    MPI_Dims_create(nranks, 2, topo->dims);
    int periods[2] = {1, 1};
    MPI_Cart_create(MPI_COMM_WORLD, 2, topo->dims, periods, 1,
                    &topo->cart_comm);
    int self;
    MPI_Comm_rank(topo->cart_comm, &self);
    MPI_Cart_coords(topo->cart_comm, self, 2, topo->coords);
    MPI_Cart_shift(topo->cart_comm, 0, 1, &topo->rank_xm, &topo->rank_xp);
    MPI_Cart_shift(topo->cart_comm, 1, 1, &topo->rank_ym, &topo->rank_yp);
}

static void setup_grid(Grid *g, const Topo *topo, int nx_gl, int ny_gl)
{
    g->nx = nx_gl / topo->dims[0];
    g->ny = ny_gl / topo->dims[1];
    int cx = topo->coords[0], cy = topo->coords[1];
    if (cx == topo->dims[0]-1) g->nx += nx_gl % topo->dims[0];
    if (cy == topo->dims[1]-1) g->ny += ny_gl % topo->dims[1];
    g->nz  = NZ_GLOBAL;
    g->ngx = g->nx + 2;
    g->ngy = g->ny + 2;
    g->dx  = LX / nx_gl;
    g->dy  = LY / ny_gl;
    g->dz  = LZ / NZ_GLOBAL;

    /*
     * Combined stability: take the minimum of all explicit terms.
     *  diffusion:  dt < dx^2 / (6 alpha),  alpha = K_eff/rho_cp
     *  radiation:  dt < rho_cp*dz / (4*sigma*eps*T_max^3),  T_max ~ T_ENV+150
     *  advection:  dt < dx / u_max,  u_max = (k/mu)*|DP_DZ|
     *  (viscous dissipation and phase change are always sub-dominant)
     */
    double K_eff = K0 * K_ANOM;
    double alpha = K_eff / RHO_CP;
    double dmin  = fmin(g->dx, fmin(g->dy, g->dz));
    double dt_diff = 0.25 * dmin * dmin / (6.0 * alpha);
    double T_max   = T_ENV + 150.0;
    double dt_rad  = 0.25 * RHO_CP * g->dz
                     / (4.0 * SIGMA_SB * EPS0 * T_max*T_max*T_max);
    double u_max   = (PERM / VISC) * fabs(DP_DZ) * 10.0;  /* ×10 for anomaly */
    double dt_adv  = (u_max > 1e-20) ? (0.25 * dmin / u_max) : dt_diff;
    g->dt = fmin(dt_diff, fmin(dt_rad, dt_adv));

    /* derived datatypes for halo faces */
    MPI_Type_contiguous(g->ny * g->nz, MPI_DOUBLE,
                        (MPI_Datatype*)&topo->yz_face);
    MPI_Type_commit((MPI_Datatype*)&topo->yz_face);
    MPI_Type_vector(g->nx, g->nz, (g->ny+2)*g->nz, MPI_DOUBLE,
                    (MPI_Datatype*)&topo->xz_face);
    MPI_Type_commit((MPI_Datatype*)&topo->xz_face);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FIELD INITIALISATION
 * Sets up spatially-heterogeneous K, emissivity ε, Darcy pressure
 * gradients (→ velocities), initial temperature, and heat source field.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void init_fields(const Grid *g, const Topo *topo,
                        double *T, double *phi,
                        double *Kx, double *Ky, double *Kz,
                        double *eps,
                        double *Px, double *Py, double *Pz,
                        double *ux, double *uy, double *uz,
                        double *Q,
                        double dx_g, double dy_g)
{
    int cx = topo->coords[0], cy = topo->coords[1];
    int nx_base = NX_GLOBAL / topo->dims[0];
    int ny_base = NY_GLOBAL / topo->dims[1];
    int x0 = cx * nx_base;
    int y0 = cy * ny_base;

    for (int ix = 1; ix <= g->nx; ix++) {
        double x = (x0 + ix - 1 + 0.5) * dx_g;
        for (int iy = 1; iy <= g->ny; iy++) {
            double y = (y0 + iy - 1 + 0.5) * dy_g;
            for (int iz = 0; iz < g->nz; iz++) {
                double z = (iz + 0.5) * g->dz;
                size_t c = IDX(g, ix, iy, iz);

                /* ── temperature: Gaussian hot spot + geothermal gradient ── */
                double r2 = pow(x-0.5*LX,2)+pow(y-0.5*LY,2)+pow(z-0.25*LZ,2);
                T[c] = T_ENV + 80.0 * exp(-r2 / (2.0*pow(0.08,2)))
                             + 100.0 * (LZ - z) / LZ;   /* geothermal grad */

                /* ── liquid fraction: zero everywhere initially ──────────── */
                phi[c] = 0.0;

                /* ── thermal conductivity (W/m·K) ────────────────────────── *
                 * Heterogeneous structure:
                 *   background K0 in all directions
                 *   high-K dyke (vertical sheet at x≈0.3)
                 *   low-K clay lens (oblique ellipsoid)
                 *   fractured zone (high-K channel at y≈0.7)
                 *   anisotropy: Kz = 0.1*Kxy in layered rock
                 */
                double K_base = K0;

                /* high-K igneous dyke — vertical, elongated in Z */
                double d_dyke = sqrt(pow(x-0.3*LX,2) + pow(y-0.5*LY,2));
                if (d_dyke < 0.04) K_base = K0 * K_ANOM * 3.0;

                /* low-K clay lens — tilted ellipsoid */
                double xr = (x-0.6*LX)*cos(0.3) + (z-0.3*LZ)*sin(0.3);
                double zr = -(x-0.6*LX)*sin(0.3) + (z-0.3*LZ)*cos(0.3);
                double r_lens = pow(xr/0.15,2) + pow(y-0.65*LY,2)/pow(0.1,2)
                              + pow(zr/0.05,2);
                if (r_lens < 1.0) K_base = K0 * 0.05;

                /* high-K fractured channel at y≈0.7 */
                if (fabs(y - 0.7*LY) < 0.03) K_base = fmax(K_base, K0*4.0);

                Kx[c] = K_base;
                Ky[c] = K_base;
                Kz[c] = K_base * 0.1;          /* vertical anisotropy */

                /* ── emissivity: spatially varying surface coating ────────── *
                 * Rock: ε≈0.95; oxidised metal: ε≈0.8; polished surface: ε≈0.1
                 * Model as a smooth spatial variation to make compute_radiation
                 * non-trivial (avoids constant-folding optimisation).
                 */
                eps[c] = EPS0 * (1.0 - 0.6 * exp(-pow(x-0.4*LX,2)/0.04)
                                       * exp(-pow(y-0.6*LY,2)/0.04));

                /* ── pressure field → Darcy velocity ─────────────────────── *
                 * Superposition of:
                 *  (a) linear hydrostatic gradient ∂P/∂z = DP_DZ
                 *  (b) over-pressured dome at (0.5, 0.5, 0.4) — Gaussian
                 *  (c) sub-pressure zone at (0.2, 0.8, 0.2)
                 * u = -(k/μ) ∇P   (Darcy's law)
                 */
                double P_dome  = 5.0e3 *
                    exp(-(pow(x-0.5*LX,2)+pow(y-0.5*LY,2)
                         +pow(z-0.4*LZ,2)) / (2.0*pow(0.1,2)));
                double P_sink  = -2.5e3 *
                    exp(-(pow(x-0.2*LX,2)+pow(y-0.8*LY,2)
                         +pow(z-0.2*LZ,2)) / (2.0*pow(0.08,2)));

                /* analytic gradient of the Gaussian anomalies */
                double dPdx = P_dome*(-(x-0.5*LX)/pow(0.1,2))
                            + P_sink*(-(x-0.2*LX)/pow(0.08,2));
                double dPdy = P_dome*(-(y-0.5*LY)/pow(0.1,2))
                            + P_sink*(-(y-0.8*LY)/pow(0.08,2));
                double dPdz = DP_DZ
                            + P_dome*(-(z-0.4*LZ)/pow(0.1,2))
                            + P_sink*(-(z-0.2*LZ)/pow(0.08,2));

                /* store gradients for use in Darcy advection */
                Px[c] = dPdx;
                Py[c] = dPdy;
                Pz[c] = dPdz;

                /* u = -(k/μ) ∇P, with permeability anisotropy */
                double k_horiz = PERM;
                double k_vert  = PERM * 0.01;  /* tight cap rock */
                ux[c] = -(k_horiz / VISC) * dPdx;
                uy[c] = -(k_horiz / VISC) * dPdy;
                uz[c] = -(k_vert  / VISC) * dPdz;

                /* ── volumetric heat source: ring of hydrothermal vents ──── */
                double r_ring = sqrt(pow(x-0.5*LX,2)+pow(y-0.5*LY,2));
                Q[c] = Q_PEAK
                    * exp(-pow(r_ring - 0.25*LX, 2) / (2.0*pow(0.04,2)))
                    * exp(-pow(z  - 0.10*LZ,      2) / (2.0*pow(0.03,2)));
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HALO EXCHANGE
 * ═══════════════════════════════════════════════════════════════════════════ */

static void halo_exchange_x(double *T, const Grid *g, const Topo *t)
{
    MPI_Status st;
    MPI_Sendrecv(T + IDX(g, g->nx,    0, 0), 1, t->yz_face, t->rank_xp, 10,
                 T + IDX(g, 0,        0, 0), 1, t->yz_face, t->rank_xm, 10,
                 t->cart_comm, &st);
    MPI_Sendrecv(T + IDX(g, 1,        0, 0), 1, t->yz_face, t->rank_xm, 11,
                 T + IDX(g, g->ngx-1, 0, 0), 1, t->yz_face, t->rank_xp, 11,
                 t->cart_comm, &st);
}

static void halo_exchange_y(double *T, const Grid *g, const Topo *t)
{
    MPI_Status st;
    MPI_Sendrecv(T + IDX(g, 0, g->ny,    0), 1, t->xz_face, t->rank_yp, 20,
                 T + IDX(g, 0, 0,        0), 1, t->xz_face, t->rank_ym, 20,
                 t->cart_comm, &st);
    MPI_Sendrecv(T + IDX(g, 0, 1,        0), 1, t->xz_face, t->rank_ym, 21,
                 T + IDX(g, 0, g->ngy-1, 0), 1, t->xz_face, t->rank_yp, 21,
                 t->cart_comm, &st);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PHYSICS KERNELS — each is a distinct profiling hot spot
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── 1. Anisotropic diffusion: ∇·(K∇T) / ρcₚ ─────────────────────────── *
 *
 * Uses arithmetic face averages for conductivity:
 *   K_{i+½} = (K_i + K_{i+1}) / 2
 *
 * This correctly handles discontinuities in K (e.g. across the dyke
 * interface) without requiring harmonic averaging, which would be more
 * accurate at sharp interfaces but adds a division per face (8 divides
 * vs 6 multiplies per cell) — arithmetic is standard for smooth fields.
 *
 * FLOPs per cell: ~40 (6 face K, 6 differences, 6 multiplies, 6 adds)
 */
static void compute_diffusion(const double *T,
                               const double *Kx, const double *Ky,
                               const double *Kz,
                               double *dT, const Grid *g)
{
    const double idx2 = 1.0 / (g->dx * g->dx);
    const double idy2 = 1.0 / (g->dy * g->dy);
    const double idz2 = 1.0 / (g->dz * g->dz);
    const double ircp = 1.0 / RHO_CP;

    for (int ix = 1; ix <= g->nx; ix++) {
        for (int iy = 1; iy <= g->ny; iy++) {
            for (int iz = 1; iz < g->nz - 1; iz++) {
                size_t c   = IDX(g, ix,   iy,   iz);
                size_t cxp = IDX(g, ix+1, iy,   iz);
                size_t cxm = IDX(g, ix-1, iy,   iz);
                size_t cyp = IDX(g, ix,   iy+1, iz);
                size_t cym = IDX(g, ix,   iy-1, iz);
                size_t czp = IDX(g, ix,   iy,   iz+1);
                size_t czm = IDX(g, ix,   iy,   iz-1);

                double Kxp = 0.5*(Kx[c]+Kx[cxp]);  double Kxm = 0.5*(Kx[c]+Kx[cxm]);
                double Kyp = 0.5*(Ky[c]+Ky[cyp]);  double Kym = 0.5*(Ky[c]+Ky[cym]);
                double Kzp = 0.5*(Kz[c]+Kz[czp]);  double Kzm = 0.5*(Kz[c]+Kz[czm]);

                dT[c] += ircp * (
                    idx2*(Kxp*(T[cxp]-T[c]) - Kxm*(T[c]-T[cxm])) +
                    idy2*(Kyp*(T[cyp]-T[c]) - Kym*(T[c]-T[cym])) +
                    idz2*(Kzp*(T[czp]-T[c]) - Kzm*(T[c]-T[czm]))
                );
            }
        }
    }
}

/* ── 2. Stefan-Boltzmann radiative cooling: σε(T_env⁴ − T⁴) / ρcₚ ──── *
 *
 * The T⁴ term makes this genuinely expensive: pow(T,4) or T*T*T*T
 * costs 3 multiplies per cell for T⁴, plus 1 for T_env⁴ (precomputed).
 * With spatially-varying ε this cannot be fused with any other kernel.
 *
 * Physics: at 350K, σT⁴ ≈ 850 W/m² — significant next to conduction
 * of K0·|∇T|/Δx ≈ 1e-4 * 100 / 1e-3 ≈ 10 W/m². We scale by a surface-
 * area-to-volume ratio (1/dz for the top face) to convert W/m² → W/m³.
 * For now treat as a volumetric sink proportional to the emissive power.
 *
 * FLOPs per cell: ~12 (T², T⁴, T_env⁴-T⁴, multiply ε, multiply σ/ρcₚdz)
 */
static void compute_radiation(const double *T, const double *eps,
                               double *dT, const Grid *g)
{
    const double sigma_over_rhocp_dz = SIGMA_SB / (RHO_CP * g->dz);
    const double Tenv4 = T_ENV*T_ENV*T_ENV*T_ENV;

    for (int ix = 1; ix <= g->nx; ix++) {
        for (int iy = 1; iy <= g->ny; iy++) {
            for (int iz = 1; iz < g->nz - 1; iz++) {
                size_t c = IDX(g, ix, iy, iz);
                double Tc  = T[c];
                double Tc2 = Tc * Tc;
                double Tc4 = Tc2 * Tc2;
                dT[c] += sigma_over_rhocp_dz * eps[c] * (Tenv4 - Tc4);
            }
        }
    }
}

/* ── 3. Darcy-flow advection: −u·∇T (upwind scheme) ─────────────────── *
 *
 * First-order upwind differencing: stable for any Péclet number and
 * guarantees no oscillations across conductivity jumps. The upwind
 * choice (which face to use) is decided by sign(u) at every cell and
 * every direction — this is a branch-heavy, memory-bandwidth-bound loop.
 *
 * u = -(k/μ)∇P (Darcy's law, already computed and stored in ux/uy/uz).
 *
 * The advected quantity is T (enthalpy transport by fluid flow). In a
 * full reservoir simulator you'd also advect species concentration and
 * saturation — T is the most expensive single field.
 *
 * FLOPs per cell: ~18 (3 directions × upwind select + multiply + add)
 * Memory: 3 reads of T-neighbours, 3 reads of u — bandwidth-limited.
 */
static void compute_darcy_advection(const double *T,
                                     const double *ux,
                                     const double *uy,
                                     const double *uz,
                                     double *dT, const Grid *g)
{
    const double idx = 1.0 / g->dx;
    const double idy = 1.0 / g->dy;
    const double idz = 1.0 / g->dz;

    for (int ix = 1; ix <= g->nx; ix++) {
        for (int iy = 1; iy <= g->ny; iy++) {
            for (int iz = 1; iz < g->nz - 1; iz++) {
                size_t c   = IDX(g, ix,   iy,   iz);
                size_t cxp = IDX(g, ix+1, iy,   iz);
                size_t cxm = IDX(g, ix-1, iy,   iz);
                size_t cyp = IDX(g, ix,   iy+1, iz);
                size_t cym = IDX(g, ix,   iy-1, iz);
                size_t czp = IDX(g, ix,   iy,   iz+1);
                size_t czm = IDX(g, ix,   iy,   iz-1);

                /* upwind: use upstream value of T based on flow direction */
                double dTdx = (ux[c] >= 0.0) ? (T[c]-T[cxm])*idx
                                              : (T[cxp]-T[c])*idx;
                double dTdy = (uy[c] >= 0.0) ? (T[c]-T[cym])*idy
                                              : (T[cyp]-T[c])*idy;
                double dTdz = (uz[c] >= 0.0) ? (T[c]-T[czm])*idz
                                              : (T[czp]-T[c])*idz;

                dT[c] -= ux[c]*dTdx + uy[c]*dTdy + uz[c]*dTdz;
            }
        }
    }
}

/* ── 4. Viscous shear heating: η|∇u|² / ρcₚ ─────────────────────────── *
 *
 * Mechanical energy is dissipated as heat wherever velocity gradients
 * are large — important in tight-zone flow, fault-zone shear, or
 * forced convection through a narrow channel.
 *
 * The full viscous dissipation rate tensor in 3D:
 *   Φ = η [2(∂u/∂x)² + 2(∂v/∂y)² + 2(∂w/∂z)²
 *          + (∂u/∂y+∂v/∂x)² + (∂u/∂z+∂w/∂x)² + (∂v/∂z+∂w/∂y)²]
 *
 * We compute all 6 strain-rate components using centred differences,
 * making this one of the most FP-intensive kernels in the code:
 * ~54 FLOPs/cell (12 differences, 6 squared, 4 cross-term squares,
 * 6 additions, scale).
 *
 * For Darcy flow in a porous medium, the correct expression is actually
 * the Brinkman term: Φ = (η/k)|u|², but the full tensor form is more
 * instructive for the profiler and is used in free-flow regions.
 */
static void compute_viscous_dissipation(const double *ux,
                                         const double *uy,
                                         const double *uz,
                                         double *dT, const Grid *g)
{
    const double idx  = 1.0 / (2.0 * g->dx);   /* central diff prefactor */
    const double idy  = 1.0 / (2.0 * g->dy);
    const double idz  = 1.0 / (2.0 * g->dz);
    const double scale = ETA_VIS / RHO_CP;

    for (int ix = 1; ix <= g->nx; ix++) {
        for (int iy = 1; iy <= g->ny; iy++) {
            for (int iz = 1; iz < g->nz - 1; iz++) {
                size_t c   = IDX(g, ix,   iy,   iz);
                size_t cxp = IDX(g, ix+1, iy,   iz);
                size_t cxm = IDX(g, ix-1, iy,   iz);
                size_t cyp = IDX(g, ix,   iy+1, iz);
                size_t cym = IDX(g, ix,   iy-1, iz);
                size_t czp = IDX(g, ix,   iy,   iz+1);
                size_t czm = IDX(g, ix,   iy,   iz-1);

                /* diagonal strain rates */
                double exx = (ux[cxp]-ux[cxm])*idx;
                double eyy = (uy[cyp]-uy[cym])*idy;
                double ezz = (uz[czp]-uz[czm])*idz;

                /* off-diagonal (shear) strain rates */
                double exy = (ux[cyp]-ux[cym])*idy + (uy[cxp]-uy[cxm])*idx;
                double exz = (ux[czp]-ux[czm])*idz + (uz[cxp]-uz[cxm])*idx;
                double eyz = (uy[czp]-uy[czm])*idz + (uz[cyp]-uz[cym])*idy;

                /* dissipation rate: Φ = η[2(exx²+eyy²+ezz²)+exy²+exz²+eyz²] */
                double Phi = 2.0*(exx*exx + eyy*eyy + ezz*ezz)
                           + exy*exy + exz*exz + eyz*eyz;

                dT[c] += scale * Phi;
            }
        }
    }
}

/* ── 5. Phase change: sequential enthalpy method ──────────────────── *
 *
 * Called AFTER advance() has already updated T. Corrects T for latent
 * heat and updates the liquid fraction φ ∈ [0,1].
 *
 * Algorithm (Voller & Prakash 1987, simplified explicit form):
 *
 *   1. Compute equilibrium φ_eq from current T (mushy-zone model):
 *        φ_eq = 0              if T < T_melt - T_range
 *        φ_eq = (T−Tmelt+Tr) / (2·Tr)   in mushy zone
 *        φ_eq = 1              if T > T_melt + T_range
 *
 *   2. Drive φ toward φ_eq:
 *        φ_new = clamp(φ_old + (φ_eq − φ_old), 0, 1)
 *        (full single-step relaxation; stable when dt << L/cp/|dT/dt|)
 *
 *   3. Correct T for latent heat released/absorbed:
 *        T_corrected = T − dphi * STEFAN
 *      where STEFAN = ρL/(ρcₚ) = L/cₚ [K] is the temperature
 *      change equivalent of complete melting.
 *      Sign: melting (dphi>0) absorbs heat → cools the cell;
 *            freezing (dphi<0) releases heat → warms the cell.
 *
 * This is branch-heavy and its cost scales with the fraction of the
 * domain inside the mushy zone (T_melt ± T_range), making it a
 * time-varying hot spot — interesting for the profiler to track
 * across timesteps as the melt front advances.
 *
 * FLOPs per cell: ~15 (comparison, linear interp, clamp, multiply, subtract)
 */
static void compute_phase_change(double *T, double *phi,
                                  double *dT, const Grid *g, double dt)
{
    const double inv_range = 1.0 / (2.0 * T_RANGE);
    (void)dT; (void)dt;

    for (int ix = 1; ix <= g->nx; ix++) {
        for (int iy = 1; iy <= g->ny; iy++) {
            for (int iz = 1; iz < g->nz - 1; iz++) {
                size_t c = IDX(g, ix, iy, iz);
                double Tc   = T[c];
                double phic = phi[c];

                /* equilibrium liquid fraction at this temperature */
                double phi_eq;
                if      (Tc <= T_MELT - T_RANGE) phi_eq = 0.0;
                else if (Tc >= T_MELT + T_RANGE)  phi_eq = 1.0;
                else    phi_eq = (Tc - T_MELT + T_RANGE) * inv_range;

                /* relax phi toward equilibrium in one step */
                double phi_new = phic + (phi_eq - phic);
                phi_new = phi_new < 0.0 ? 0.0 : (phi_new > 1.0 ? 1.0 : phi_new);
                double dphi_actual = phi_new - phic;
                phi[c] = phi_new;

                /* correct T for latent heat:
                 *   melting absorbs STEFAN [K] per unit change in phi → T drops
                 *   freezing releases heat  → T rises
                 * This keeps the total enthalpy H = ρcₚT + ρL·phi conserved. */
                T[c] -= dphi_actual * STEFAN;
            }
        }
    }
}

/* ── 6. Volumetric heat source ──────────────────────────────────────── */
static void update_source(double *dT, const double *Q,
                           const Grid *g, double pulse)
{
    const double ircp = 1.0 / RHO_CP;
    for (int ix = 1; ix <= g->nx; ix++)
        for (int iy = 1; iy <= g->ny; iy++)
            for (int iz = 1; iz < g->nz - 1; iz++) {
                size_t c = IDX(g, ix, iy, iz);
                dT[c] += ircp * pulse * Q[c];
            }
}

/* ── 7. Boundary conditions ─────────────────────────────────────────── *
 * Dirichlet on Z: bottom (iz=0) is hot geothermal base, top is cold.
 * The mushy-zone BC ensures phi is consistent with the imposed T.
 */
static void apply_bc(double *T, double *phi, const Grid *g)
{
    double T_bottom = T_ENV + 100.0;   /* hot geothermal base [K]       */
    double T_top    = T_ENV;           /* cold surface [K]              */
    double T_min_physical = 0.1;       /* below this is unphysical      */

    for (int ix = 0; ix < g->ngx; ix++) {
        for (int iy = 0; iy < g->ngy; iy++) {
            size_t cb = IDX(g, ix, iy, 0);
            size_t ct = IDX(g, ix, iy, g->nz-1);
            T[cb]   = T_bottom;  phi[cb] = 0.0;
            T[ct]   = T_top;     phi[ct] = 0.0;
        }
    }

    /* Physical floor: explicit radiation can temporarily overshoot on
       the first few steps before the temperature field has equilibrated.
       Clamp interior cells to T_min_physical to avoid negative T^4. */
    for (int ix = 1; ix <= g->nx; ix++)
        for (int iy = 1; iy <= g->ny; iy++)
            for (int iz = 1; iz < g->nz-1; iz++) {
                size_t c = IDX(g, ix, iy, iz);
                if (T[c] < T_min_physical) {
                    T[c]   = T_min_physical;
                    phi[c] = 0.0;
                }
            }
}

/* ── 8. Explicit Euler advance ──────────────────────────────────────── */
static void advance(double *T, const double *dT, const Grid *g, double dt)
{
    for (int ix = 1; ix <= g->nx; ix++)
        for (int iy = 1; iy <= g->ny; iy++)
            for (int iz = 1; iz < g->nz-1; iz++) {
                size_t c = IDX(g, ix, iy, iz);
                T[c] += dt * dT[c];
            }
}

/* ── 9. Global diagnostics ──────────────────────────────────────────── */
static void compute_diagnostics(const double *T, const double *phi,
                                  const Grid *g, const Topo *t,
                                  int step, double time)
{
    double loc[4] = {0.0, 1e30, -1e30, 0.0}; /* l2, Tmin, Tmax, phi_mean */
    for (int ix = 1; ix <= g->nx; ix++) {
        for (int iy = 1; iy <= g->ny; iy++) {
            for (int iz = 1; iz < g->nz-1; iz++) {
                size_t c = IDX(g, ix, iy, iz);
                double v = T[c];
                loc[0] += v*v;
                if (v < loc[1]) loc[1] = v;
                if (v > loc[2]) loc[2] = v;
                loc[3] += phi[c];
            }
        }
    }

    double glb[4];
    /* gather L2 and phi_mean via SUM, Tmin via MIN, Tmax via MAX */
    MPI_Allreduce(&loc[0], &glb[0], 1, MPI_DOUBLE, MPI_SUM, t->cart_comm);
    MPI_Allreduce(&loc[1], &glb[1], 1, MPI_DOUBLE, MPI_MIN, t->cart_comm);
    MPI_Allreduce(&loc[2], &glb[2], 1, MPI_DOUBLE, MPI_MAX, t->cart_comm);
    MPI_Allreduce(&loc[3], &glb[3], 1, MPI_DOUBLE, MPI_SUM, t->cart_comm);

    int myrank;
    MPI_Comm_rank(t->cart_comm, &myrank);
    if (myrank == 0) {
        long ntot = (long)NX_GLOBAL * NY_GLOBAL * (NZ_GLOBAL-2);
        printf("  step=%5d t=%7.4f L2=%10.4e Tmin=%6.1f Tmax=%6.1f"
               " phi_mean=%.4f\n",
               step, time, sqrt(glb[0]),
               glb[1], glb[2], glb[3]/ntot);
    }
}

/* ── 10. Mid-plane slice output ─────────────────────────────────────── */
static void write_slice(const double *T, const Grid *g, const Topo *t,
                         int step)
{
    int iz_mid = g->nz / 2;
    int local_count = g->nx * g->ny;
    double *local_slice = malloc((size_t)local_count * sizeof(double));
    int k = 0;
    for (int ix = 1; ix <= g->nx; ix++)
        for (int iy = 1; iy <= g->ny; iy++)
            local_slice[k++] = T[IDX(g, ix, iy, iz_mid)];

    int myrank, nranks;
    MPI_Comm_rank(t->cart_comm, &myrank);
    MPI_Comm_size(t->cart_comm, &nranks);

    int    *counts = malloc(nranks * sizeof(int));
    int    *displs = malloc(nranks * sizeof(int));
    double *global_slice = NULL;

    MPI_Gather(&local_count, 1, MPI_INT, counts, 1, MPI_INT, 0, t->cart_comm);
    if (myrank == 0) {
        displs[0] = 0;
        for (int r = 1; r < nranks; r++) displs[r] = displs[r-1]+counts[r-1];
        global_slice = malloc((size_t)(displs[nranks-1]+counts[nranks-1])
                              * sizeof(double));
    }
    MPI_Gatherv(local_slice, local_count, MPI_DOUBLE,
                global_slice, counts, displs, MPI_DOUBLE, 0, t->cart_comm);

    if (myrank == 0) {
        double csum = 0.0;
        int total = displs[nranks-1]+counts[nranks-1];
        for (int i = 0; i < total; i++) csum += global_slice[i];
        printf("# slice z=%d step=%d checksum=%.8e\n", iz_mid, step, csum);
    }

    free(local_slice); free(global_slice); free(counts); free(displs);
}
