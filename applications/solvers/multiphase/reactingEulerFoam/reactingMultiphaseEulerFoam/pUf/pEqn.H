PtrList<surfaceScalarField> alphafs(phases.size());
PtrList<surfaceScalarField> alphaRho0fs(phases.size());
PtrList<surfaceScalarField> rAUfs(phases.size());
PtrList<surfaceScalarField> alpharAUfs(phases.size());

forAll(phases, phasei)
{
    phaseModel& phase = phases[phasei];
    const volScalarField& alpha = phase;

    alphafs.set(phasei, fvc::interpolate(alpha).ptr());
    alphafs[phasei].rename("pEqn" + alphafs[phasei].name());

    alphaRho0fs.set
    (
        phasei,
        (
            fvc::interpolate
            (
                max(alpha.oldTime(), phase.residualAlpha())
               *phase.rho()().oldTime()
            )
        ).ptr()
    );

    // add implicit part of drag and virtual mass force
    rAUfs.set
    (
        phasei,
        new surfaceScalarField
        (
            IOobject::groupName("rAUf", phase.name()),
            1.0
           /(
                byDt(alphaRho0fs[phasei] + fluid.Vmf(phase))
              + fvc::interpolate(UEqns[phasei].A())
              + fluid.Kdf(phase)
            )
        )
    );

    alpharAUfs.set
    (
        phasei,
        (
            max(alphafs[phasei], phase.residualAlpha())*rAUfs[phasei]
        ).ptr()
    );
}

// Lift, wall-lubrication and turbulent diffusion fluxes
PtrList<surfaceScalarField> phiFs(phases.size());
{
    autoPtr<PtrList<surfaceScalarField>> Fs = fluid.Ffs();

    forAll(phases, phasei)
    {
        phaseModel& phase = phases[phasei];

        if (Fs().set(phasei))
        {
            phiFs.set
            (
                phasei,
                new surfaceScalarField
                (
                    IOobject::groupName("phiF", phase.name()),
                    rAUfs[phasei]*Fs()[phasei]
                )
            );
        }
    }
}
{
    autoPtr<PtrList<surfaceScalarField>> phiDs = fluid.phiDfs(rAUfs);

    forAll(phases, phasei)
    {
        phaseModel& phase = phases[phasei];

        if (phiDs().set(phasei))
        {
            if (phiFs.set(phasei))
            {
                phiFs[phasei] += phiDs()[phasei];
            }
            else
            {
                phiFs.set
                (
                    phasei,
                    new surfaceScalarField
                    (
                        IOobject::groupName("phiF", phase.name()),
                        phiDs()[phasei]
                    )
                );
            }
        }
    }
}

// --- Pressure corrector loop
while (pimple.correct())
{
    volScalarField rho("rho", fluid.rho());

    // Correct p_rgh for consistency with p and the updated densities
    p_rgh = p - rho*gh;

    forAll(phases, phasei)
    {
        phaseModel& phase = phases[phasei];

        // Correct fixed-flux BCs to be consistent with the velocity BCs
        MRF.correctBoundaryFlux(phase.U(), phase.phi());
    }

    surfaceScalarField ghSnGradRho
    (
        "ghSnGradRho",
        ghf*fvc::snGrad(rho)*mesh.magSf()
    );

    PtrList<surfaceScalarField> phigFs(phases.size());
    forAll(phases, phasei)
    {
        phaseModel& phase = phases[phasei];

        phigFs.set
        (
            phasei,
            (
                alpharAUfs[phasei]
               *(
                   ghSnGradRho
                 - (fvc::interpolate(phase.rho() - rho))*(g & mesh.Sf())
                 - fluid.surfaceTension(phase)*mesh.magSf()
                )
            ).ptr()
        );

        if (phiFs.set(phasei))
        {
            phigFs[phasei] += phiFs[phasei];
        }
    }

    PtrList<surfaceScalarField> phiHbyAs(phases.size());

    surfaceScalarField phiHbyA
    (
        IOobject
        (
            "phiHbyA",
            runTime.timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("phiHbyA", dimArea*dimVelocity, 0)
    );

    forAll(phases, phasei)
    {
        phaseModel& phase = phases[phasei];

        phiHbyAs.set
        (
            phasei,
            new surfaceScalarField
            (
                IOobject::groupName("phiHbyA", phase.name()),
                rAUfs[phasei]
               *(
                    fvc::flux(UEqns[phasei].H())
                  + alphaRho0fs[phasei]
                   *byDt(MRF.absolute(phase.phi().oldTime()))
                )
              - phigFs[phasei]
            )
        );

        // Add explicit part of the drag
        forAllConstIter
        (
            phaseSystem::KdfTable,
            fluid.Kdfs(),
            KdfIter
        )
        {
            const surfaceScalarField& Kf(*KdfIter());

            const phasePair& pair(fluid.phasePairs()[KdfIter.key()]);

            if (pair.contains(phase))
            {
                phiHbyAs[phasei] +=
                    rAUfs[phasei]*Kf*MRF.absolute(pair.otherPhase(phase).phi());
            }
        }

        // Add explicit part of the virtual mass force
        forAllConstIter
        (
            phaseSystem::VmfTable,
            fluid.Vmfs(),
            VmfIter
        )
        {
            const surfaceScalarField& Vmf(*VmfIter());

            const phasePair& pair(fluid.phasePairs()[VmfIter.key()]);

            if (pair.contains(phase))
            {
                phiHbyAs[phasei] +=
                    rAUfs[phasei]
                   *(
                        Vmf*byDt(MRF.absolute(phase.phi().oldTime()))
                      + Vmf*ddtPhis[pair.otherPhase(phase).index()]
                    );
            }
        }

        phiHbyA += alphafs[phasei]*phiHbyAs[phasei];
    }

    MRF.makeRelative(phiHbyA);

    // Construct pressure "diffusivity"
    surfaceScalarField rAUf
    (
        IOobject
        (
            "rAUf",
            runTime.timeName(),
            mesh
        ),
        mesh,
        dimensionedScalar("rAUf", dimensionSet(-1, 3, 1, 0, 0), 0)
    );

    forAll(phases, phasei)
    {
        rAUf += alphafs[phasei]*alpharAUfs[phasei];
    }
    rAUf = mag(rAUf);


    // Update the fixedFluxPressure BCs to ensure flux consistency
    {
        surfaceScalarField::Boundary phib(phi.boundaryField());
        phib = 0;
        forAll(phases, phasei)
        {
            phaseModel& phase = phases[phasei];
            phib += alphafs[phasei].boundaryField()*phase.phi().boundaryField();
        }

        setSnGrad<fixedFluxPressureFvPatchScalarField>
        (
            p_rgh.boundaryFieldRef(),
            (
                phiHbyA.boundaryField() - phib
            )/(mesh.magSf().boundaryField()*rAUf.boundaryField())
        );
    }

    PtrList<fvScalarMatrix> pEqnComps(phases.size());
    forAll(phases, phasei)
    {
        phaseModel& phase = phases[phasei];
        const volScalarField& alpha = phase;
        volScalarField& rho = phase.thermo().rho();

        if (phase.compressible())
        {
            if (pimple.transonic())
            {
                surfaceScalarField phid
                (
                    IOobject::groupName("phid", phase.name()),
                    fvc::interpolate(phase.thermo().psi())*phase.phi()
                );

                pEqnComps.set
                (
                    phasei,
                    (
                        (
                            phase.continuityError()
                          - fvc::Sp
                            (
                                fvc::ddt(alpha) + fvc::div(phase.alphaPhi()),
                                rho
                            )
                        )/rho
                      + correction
                        (
                            (alpha/rho)*
                            (
                                phase.thermo().psi()*fvm::ddt(p_rgh)
                              + fvm::div(phid, p_rgh)
                              - fvm::Sp(fvc::div(phid), p_rgh)
                            )
                        )
                    ).ptr()
                );

                deleteDemandDrivenData
                (
                    pEqnComps[phasei].faceFluxCorrectionPtr()
                );
                pEqnComps[phasei].relax();
            }
            else
            {
                pEqnComps.set
                (
                    phasei,
                    (
                        (
                            phase.continuityError()
                          - fvc::Sp
                            (
                                (fvc::ddt(alpha) + fvc::div(phase.alphaPhi())),
                                rho
                            )
                        )/rho
                      + (alpha*phase.thermo().psi()/rho)
                       *correction(fvm::ddt(p_rgh))
                    ).ptr()
                );
            }
        }
        else
        {
            pEqnComps.set
            (
                phasei,
                fvm::Su(-(fvOptions(alpha, rho)&rho)/rho, p_rgh).ptr()
            );
        }

        if (fluid.transfersMass(phase))
        {
            if (pEqnComps.set(phasei))
            {
                pEqnComps[phasei] -= fluid.dmdt(phase)/rho;
            }
            else
            {
                pEqnComps.set
                (
                    phasei,
                    fvm::Su(-fluid.dmdt(phase)/rho, p_rgh)
                );
            }
        }
    }

    // Cache p prior to solve for density update
    volScalarField p_rgh_0(p_rgh);

    // Iterate over the pressure equation to correct for non-orthogonality
    while (pimple.correctNonOrthogonal())
    {
        // Construct the transport part of the pressure equation
        fvScalarMatrix pEqnIncomp
        (
            fvc::div(phiHbyA)
          - fvm::laplacian(rAUf, p_rgh)
        );

        {
            fvScalarMatrix pEqn(pEqnIncomp);

            forAll(phases, phasei)
            {
                if (pEqnComps.set(phasei))
                {
                    pEqn += pEqnComps[phasei];
                }
            }

            solve
            (
                pEqn,
                mesh.solver(p_rgh.select(pimple.finalInnerIter()))
            );
        }

        // Correct fluxes and velocities on last non-orthogonal iteration
        if (pimple.finalNonOrthogonalIter())
        {
            phi = phiHbyA + pEqnIncomp.flux();

            surfaceScalarField mSfGradp("mSfGradp", pEqnIncomp.flux()/rAUf);

            forAll(phases, phasei)
            {
                phaseModel& phase = phases[phasei];

                phase.phi() = phiHbyAs[phasei] + alpharAUfs[phasei]*mSfGradp;

                // Set the phase dilatation rates
                if (pEqnComps.set(phasei))
                {
                    phase.divU(-pEqnComps[phasei] & p_rgh);
                }
            }

            // Optionally relax pressure for velocity correction
            p_rgh.relax();

            mSfGradp = pEqnIncomp.flux()/rAUf;

            forAll(phases, phasei)
            {
                phaseModel& phase = phases[phasei];

                phase.U() = fvc::reconstruct(MRF.absolute(phase.phi()));
                phase.U().correctBoundaryConditions();
                fvOptions.correct(phase.U());
            }
        }
    }

    // Update and limit the static pressure
    p = max(p_rgh + rho*gh, pMin);

    // Limit p_rgh
    p_rgh = p - rho*gh;

    // Update densities from change in p_rgh
    forAll(phases, phasei)
    {
        phaseModel& phase = phases[phasei];
        phase.thermo().rho() += phase.thermo().psi()*(p_rgh - p_rgh_0);
    }

    // Correct p_rgh for consistency with p and the updated densities
    rho = fluid.rho();
    p_rgh = p - rho*gh;
    p_rgh.correctBoundaryConditions();
}
