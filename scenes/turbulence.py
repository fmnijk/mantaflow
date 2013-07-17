#
# Example scene for mesh-based smoke simulations using Vortex Sheets
# ( http://graphics.ethz.ch/publications/papers/paperPfa12.php )
#
# This example requires CUDA to run

from manta import *

scale = 0.2

# solver params
res = 64
gs = vec3(res,res/2,res/2)
s = Solver(name='main', gridSize = gs)
s.timestep = 0.5

velInflow = vec3(0.52,0,0)

# prepare grids
flags = s.create(FlagGrid)
pressure = s.create(RealGrid, show=False)
vel = s.create(MACGrid)

k = s.create(RealGrid)
eps = s.create(RealGrid)
prod = s.create(RealGrid)
nuT= s.create(RealGrid)
strain= s.create(RealGrid)
vc=s.create(MACGrid)
temp=s.create(RealGrid)

# noise field
noise = s.create(NoiseField)
noise.timeAnim = 0

# turbulence particles
turb = s.create(TurbulenceParticleSystem, noise=noise)

flags.initDomain()
flags.fillGrid()

# obstacle grid
for i in range(4):
    for j in range(4):
        obs = s.create(Sphere, center=gs*vec3(0.2,(i+1)/5.0,(j+1)/5.0), radius=res*0.025)
        obs.applyToGrid(grid=flags,value=FlagObstacle)

sdfgrad = obstacleGradient(flags);

# particle inflow
cyl = s.create(Cylinder, center = gs*vec3(0.05,0.5,0.5), radius=res*0.1, z=vec3(res*0.03,0,0))

# turbulence parameters
L0 = 0.03
mult = 0.1
intensity = 0.1
nu = 0.1
prodMult = 2.5
enableDiffuse = True

if (GUI):
    gui = Gui()
    gui.show()
    sliderL0 = gui.addControl(Slider, text='turbulent lengthscale', val=L0, min=0.01, max=2)
    sliderMult = gui.addControl(Slider, text='turbulent mult', val=mult, min=0, max=1)
    sliderProd = gui.addControl(Slider, text='production mult', val=prodMult, min=0.1, max=5)
    checkDiff = gui.addControl(Checkbox, text='enable grad.diff', val=enableDiffuse)

KEpsilonBcs(flags=flags,k=k,eps=eps,intensity=intensity,nu=nu,fillArea=True)

#box=s.create(Box, p0=vec3(0,0,0), p1=gs*vec3(1,1,1))
#turb.seed(box,10000)

#main loop
for t in range(10000):
    if (GUI):
        mult = sliderMult.get()
        K0 = sliderL0.get()
        enableDiffuse = checkDiff.get()
        prodMult = sliderProd.get()
    
    turb.seed(cyl,60)
    turb.advectInGrid(flaggrid=flags, vel=vel, integrationMode=IntRK4)
    turb.synthesize(flags=flags, octaves=2, k=k, switchLength=10, L0=L0, scale=mult, inflowBias=velInflow)
    turb.projectOutside(sdfgrad)
    
    KEpsilonBcs(flags=flags,k=k,eps=eps,intensity=intensity,nu=nu,fillArea=False)
    advectSemiLagrange(flags=flags, vel=vel, grid=k, order=1)
    advectSemiLagrange(flags=flags, vel=vel, grid=eps, order=1)
    KEpsilonBcs(flags=flags,k=k,eps=eps,intensity=intensity,nu=nu,fillArea=False)
    KEpsilonComputeProduction(vel=vel, k=k, eps=eps, prod=prod, nuT=nuT, strain=strain, pscale=prodMult) 
    KEpsilonSources(k=k, eps=eps, prod=prod)
    
    if enableDiffuse:
        KEpsilonGradientDiffusion(k=k, eps=eps, vel=vel, nuT=nuT, sigmaU=10.0);

    # base solver
    advectSemiLagrange(flags=flags, vel=vel, grid=vel, order=2)
    setWallBcs(flags=flags, vel=vel)
    setInflowBcs(vel=vel,dir='xXyYzZ',value=velInflow)
    solvePressure(flags=flags, vel=vel, pressure=pressure, cgMaxIterFac=0.5)
    setWallBcs(flags=flags, vel=vel)
    setInflowBcs(vel=vel,dir='xXyYzZ',value=velInflow)
    
    #s.printTimings()
    s.step()
    