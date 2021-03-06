/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2019 Synthetik Applied Technologies
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is derivative work of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "reactingCompressibleSystem.H"
#include "fvm.H"
#include "addToRunTimeSelectionTable.H"

// * * * * * * * * * * * * * Static member functions * * * * * * * * * * * * //

namespace Foam
{
    defineTypeNameAndDebug(reactingCompressibleSystem, 0);
}

// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::reactingCompressibleSystem::reactingCompressibleSystem
(
    const fvMesh& mesh,
    const dictionary& dict
)
:
    integrationSystem("phaseCompressibleSystem", mesh),
    thermo_(rhoReactionThermo::New(mesh)),
    rho_
    (
        IOobject
        (
            "rho",
            mesh.time().timeName(),
            mesh,
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("rho", dimDensity, 0.0)
    ),
    U_
    (
        IOobject
        (
            "U",
            mesh.time().timeName(),
            mesh,
            IOobject::MUST_READ,
            IOobject::AUTO_WRITE
        ),
        mesh
    ),
    p_(thermo_->p()),
    T_(thermo_->T()),
    e_(thermo_->he()),
    rhoU_
    (
        IOobject
        (
            "rhoU",
            mesh.time().timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::AUTO_WRITE
        ),
        rho_*U_
    ),
    rhoE_
    (
        IOobject
        (
            "rhoE",
            mesh.time().timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("0", dimDensity*sqr(dimVelocity), 0.0)
    ),
    phi_
    (
        IOobject
        (
            "phi",
            mesh.time().timeName(),
            mesh
        ),
        mesh,
        dimensionedScalar("0", dimVelocity*dimArea, 0.0)
    ),
    rhoPhi_
    (
        IOobject
        (
            "rhoPhi",
            mesh.time().timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("0", dimDensity*dimVelocity*dimArea, 0.0)
    ),
    rhoUPhi_
    (
        IOobject
        (
            "rhoUPhi",
            mesh.time().timeName(),
            mesh
        ),
        mesh,
        dimensionedVector("0", dimDensity*sqr(dimVelocity)*dimArea, Zero)
    ),
    rhoEPhi_
    (
        IOobject
        (
            "rhoEPhi",
            mesh.time().timeName(),
            mesh
        ),
        mesh,
        dimensionedScalar("0", dimDensity*pow3(dimVelocity)*dimArea, 0.0)
    ),
    fluxScheme_(fluxScheme::New(mesh)),
    g_(mesh.lookupObject<uniformDimensionedVectorField>("g"))
{
    thermo_->validate("compressibleSystem", "e");
    rho_ = thermo_->rho();

    if (min(thermo_->mu()).value() > small)
    {
        turbulence_.set
        (
            compressible::turbulenceModel::New
            (
                rho_,
                U_,
                phi_,
                thermo_()
            ).ptr()
        );
    }

    Switch useChemistry
    (
        word(thermo_->subDict("thermoType").lookup("mixture"))
     == "reactingMixture"
    );
    if (useChemistry)
    {
        reaction_.set
        (
            CombustionModel<rhoReactionThermo>::New
            (
                refCast<rhoReactionThermo>(thermo_()),
                turbulence_()
            ).ptr()
        );
        word inertSpecie(thermo_->lookup("inertSpecie"));
        inertIndex_ = thermo_->composition().species()[inertSpecie];
    }
    encode();
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::reactingCompressibleSystem::~reactingCompressibleSystem()
{}

// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::reactingCompressibleSystem::solve
(
    const label stepi,
    const scalarList& ai,
    const scalarList& bi
)
{
    if (oldIs_[stepi - 1] != -1)
    {
        rhoOld_.set
        (
            oldIs_[stepi - 1],
            new volScalarField(rho_)
        );
        rhoUOld_.set
        (
            oldIs_[stepi - 1],
            new volVectorField(rhoU_)
        );
        rhoEOld_.set
        (
            oldIs_[stepi - 1],
            new volScalarField(rhoE_)
        );
    }

    volScalarField rhoOld(ai[stepi - 1]*rho_);
    volVectorField rhoUOld(ai[stepi - 1]*rhoU_);
    volScalarField rhoEOld(ai[stepi - 1]*rhoE_);
    for (label i = 0; i < stepi - 1; i++)
    {
        label fi = oldIs_[i];
        if (fi != -1 && ai[fi] != 0)
        {
            rhoOld += ai[fi]*rhoOld_[fi];
            rhoUOld += ai[fi]*rhoUOld_[fi];
            rhoEOld += ai[fi]*rhoEOld_[fi];
        }
    }

    volScalarField deltaRho(fvc::div(rhoPhi_));
    volVectorField deltaRhoU(fvc::div(rhoUPhi_) - g_*rho_);
    volScalarField deltaRhoE
    (
        fvc::div(rhoEPhi_)
      - (rhoU_ & g_)
    );
    if (deltaIs_[stepi - 1] != -1)
    {
        deltaRho_.set
        (
            deltaIs_[stepi - 1],
            new volScalarField(deltaRho)
        );
        deltaRhoU_.set
        (
            deltaIs_[stepi - 1],
            new volVectorField(deltaRhoU)
        );
        deltaRhoE_.set
        (
            deltaIs_[stepi - 1],
            new volScalarField(deltaRhoE)
        );
    }
    deltaRho *= bi[stepi - 1];
    deltaRhoU *= bi[stepi - 1];
    deltaRhoE *= bi[stepi - 1];

    for (label i = 0; i < stepi - 1; i++)
    {
        label fi = deltaIs_[i];
        if (fi != -1 && bi[fi] != 0)
        {
            deltaRho += bi[fi]*deltaRho_[fi];
            deltaRhoU += bi[fi]*deltaRhoU_[fi];
            deltaRhoE += bi[fi]*deltaRhoE_[fi];
        }
    }

    dimensionedScalar dT = rho_.time().deltaT();
    rho_ = rhoOld - dT*deltaRho;
    rho_.correctBoundaryConditions();

    vector solutionDs((vector(rho_.mesh().solutionD()) + vector::one)/2.0);
    rhoU_ = cmptMultiply(rhoUOld - dT*deltaRhoU, solutionDs);
    rhoE_ = rhoEOld - dT*deltaRhoE;

    if (stepi == oldIs_.size() && (turbulence_.valid()))
    {
        U_ = rhoU_/rho_;
        U_.correctBoundaryConditions();

        volScalarField muEff("muEff", turbulence_->muEff());
        volTensorField tauMC("tauMC", muEff*dev2(Foam::T(fvc::grad(U_))));

        fvVectorMatrix UEqn
        (
            fvm::ddt(rho_, U_) - fvc::ddt(rho_, U_)
         ==
            fvm::laplacian(muEff, U_)
          + fvc::div(tauMC)
        );

        UEqn.solve();

        rhoU_ = rho_*U_;

        e_ = rhoE_/rho_ - 0.5*magSqr(U_);
        e_.correctBoundaryConditions();

        Foam::solve
        (
            fvm::ddt(rho_, e_) - fvc::ddt(rho_, e_)
        - fvm::laplacian(turbulence_->alphaEff(), e_)
        );
        rhoE_ = rho_*(e_ + 0.5*magSqr(U_)); // Includes change to total energy from viscous term in momentum equation

        turbulence_->correct();
    }

    //- Solve chemistry on the final step
    if (stepi == oldIs_.size())
    {
        if (reaction_.valid())
        {
            reaction_->correct();

            PtrList<volScalarField>& Y = thermo_->composition().Y();
            volScalarField Yt(0.0*Y[0]);
            forAll(Y, i)
            {
                if (i != inertIndex_ && thermo_->composition().active(i))
                {
                    volScalarField& Yi = Y[i];

                    fvScalarMatrix YiEqn
                    (
                        fvm::ddt(rho_, Yi)
                      + fvm::div(rhoPhi_, Yi, "div(rhoPhi,Yi)")
                      - fvm::laplacian(turbulence_->alphaEff(), Yi)
                    ==
                        reaction_->R(Yi)
                    );
                    YiEqn.solve("Yi");


                    Yi.max(0.0);
                    Yt += Yi;
                }
            }
            Y[inertIndex_] = scalar(1) - Yt;
            Y[inertIndex_].max(0.0);
            rhoE_ += dT*reaction_->Qdot();
        }
    }
    decode();
}


void Foam::reactingCompressibleSystem::setODEFields
(
    const label nSteps,
    const boolList& storeFields,
    const boolList& storeDeltas
)
{
    oldIs_.resize(nSteps);
    deltaIs_.resize(nSteps);
    label fi = 0;
    for (label i = 0; i < nSteps; i++)
    {
        if (storeFields[i])
        {
            oldIs_[i] = fi;
            fi++;
        }
        else
        {
            oldIs_[i] = -1;
        }
    }
    nOld_ = fi;
    rhoOld_.resize(nOld_);
    rhoUOld_.resize(nOld_);
    rhoEOld_.resize(nOld_);

    fi = 0;
    for (label i = 0; i < nSteps; i++)
    {
        if (storeDeltas[i])
        {
            deltaIs_[i] = fi;
            fi++;
        }
        else
        {
            deltaIs_[i] = -1;
        }
    }
    nDelta_ = fi;
    deltaRho_.resize(nDelta_);
    deltaRhoU_.resize(nDelta_);
    deltaRhoE_.resize(nDelta_);
}

void Foam::reactingCompressibleSystem::clearODEFields()
{
    fluxScheme_->clear();
    rhoOld_.clear();
    rhoUOld_.clear();
    rhoEOld_.clear();
    rhoOld_.resize(nOld_);
    rhoUOld_.resize(nOld_);
    rhoEOld_.resize(nOld_);

    deltaRho_.clear();
    deltaRhoU_.clear();
    deltaRhoE_.clear();
    deltaRho_.resize(nDelta_);
    deltaRhoU_.resize(nDelta_);
    deltaRhoE_.resize(nDelta_);
}


void Foam::reactingCompressibleSystem::update()
{
    fluxScheme_->update
    (
        rho_,
        U_,
        e_,
        p_,
        speedOfSound()(),
        phi_,
        rhoPhi_,
        rhoUPhi_,
        rhoEPhi_
    );
}


void Foam::reactingCompressibleSystem::decode()
{
    thermo_->rho() = rho_;

    U_.ref() = rhoU_()/rho_();
    U_.correctBoundaryConditions();

    rhoU_.boundaryFieldRef() = rho_.boundaryField()*U_.boundaryField();

    volScalarField E(rhoE_/rho_);
    e_.ref() = E() - 0.5*magSqr(U_());
    e_.correctBoundaryConditions();

    rhoE_.boundaryFieldRef() =
        rho_.boundaryField()
       *(
            e_.boundaryField()
          + 0.5*magSqr(U_.boundaryField())
        );

    thermo_->correct();
    p_.ref() = rho_/thermo_->psi();
    p_.correctBoundaryConditions();
    rho_.boundaryFieldRef() ==
        thermo_->psi().boundaryField()*p_.boundaryField();
}


void Foam::reactingCompressibleSystem::encode()
{
    rho_ = thermo_->rho();
    rhoU_ = rho_*U_;
    rhoE_ = rho_*(e_ + 0.5*magSqr(U_));
}


Foam::tmp<Foam::volScalarField>
Foam::reactingCompressibleSystem::speedOfSound() const
{
    return sqrt(thermo_->gamma()/thermo_->psi());
}


Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::Cv() const
{
    return thermo_->Cv();
}


Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::mu() const
{
    return thermo_->mu();
}


Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::mu(const label patchi) const
{
    return thermo_->mu(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::nu() const
{
    return thermo_->nu();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::nu(const label patchi) const
{
    return thermo_->nu(patchi);
}

Foam::tmp<Foam::volScalarField>
Foam::reactingCompressibleSystem::alpha() const
{
    return thermo_->alpha();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::alpha(const label patchi) const
{
    return thermo_->alpha(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::alphaEff
(
    const volScalarField& alphat
) const
{
    return thermo_->alphaEff(alphat);
}

Foam::tmp<Foam::scalarField> Foam::reactingCompressibleSystem::alphaEff
(
    const scalarField& alphat,
    const label patchi
) const
{
    return thermo_->alphaEff(alphat, patchi);
}

Foam::tmp<Foam::volScalarField>
Foam::reactingCompressibleSystem::alphahe() const
{
    return thermo_->alphahe();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::alphahe(const label patchi) const
{
    return thermo_->alphahe(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::kappa() const
{
    return thermo_->kappa();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::kappa(const label patchi) const
{
    return thermo_->kappa(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::kappaEff
(
    const volScalarField& alphat
) const
{
    return thermo_->kappaEff(alphat);
}

Foam::tmp<Foam::scalarField> Foam::reactingCompressibleSystem::kappaEff
(
    const scalarField& alphat,
    const label patchi
) const
{
    return thermo_->kappaEff(alphat, patchi);
}
// ************************************************************************* //
