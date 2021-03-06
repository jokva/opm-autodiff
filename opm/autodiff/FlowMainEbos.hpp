/*
  Copyright 2013, 2014, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2015 IRIS AS
  Copyright 2014 STATOIL ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED
#define OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED

#include <opm/simulators/ParallelFileMerger.hpp>

#include <opm/autodiff/BlackoilModelEbos.hpp>
#include <opm/autodiff/NewtonIterationBlackoilSimple.hpp>
#include <opm/autodiff/NewtonIterationBlackoilCPR.hpp>
#include <opm/autodiff/NewtonIterationBlackoilInterleaved.hpp>
#include <opm/autodiff/MissingFeatures.hpp>
#include <opm/autodiff/BlackoilPropsAdFromDeck.hpp>
#include <opm/autodiff/moduleVersion.hpp>
#include <opm/autodiff/ExtractParallelGridInformationToISTL.hpp>

#include <opm/core/props/satfunc/RelpermDiagnostics.hpp>

#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/common/OpmLog/EclipsePRTLog.hpp>
#include <opm/common/OpmLog/LogUtil.hpp>
#include <opm/common/ResetLocale.hpp>

#include <opm/parser/eclipse/Deck/Deck.hpp>
#include <opm/parser/eclipse/Parser/Parser.hpp>
#include <opm/parser/eclipse/Parser/ParseContext.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/IOConfig/IOConfig.hpp>
#include <opm/parser/eclipse/EclipseState/InitConfig/InitConfig.hpp>
#include <opm/parser/eclipse/EclipseState/checkDeck.hpp>

#include <ewoms/version.hh>

namespace Opm
{
    // The FlowMain class is the ebos based black-oil simulator.
    class FlowMainEbos
    {
    public:
        typedef TTAG(EclFlowProblem) TypeTag;
        typedef typename GET_PROP(TypeTag, MaterialLaw)::EclMaterialLawManager MaterialLawManager;
        typedef typename GET_PROP_TYPE(TypeTag, Simulator) EbosSimulator;
        typedef typename GET_PROP_TYPE(TypeTag, Grid) Grid;
        typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;

        typedef Opm::SimulatorFullyImplicitBlackoilEbos Simulator;
        typedef typename Simulator::ReservoirState ReservoirState;
        typedef typename Simulator::OutputWriter OutputWriter;

        /// This is the main function of Flow.
        /// It runs a complete simulation, with the given grid and
        /// simulator classes, based on user command-line input.  The
        /// content of this function used to be in the main() function of
        /// flow.cpp.
        int execute(int argc, char** argv)
        {
            try {
                // we always want to use the default locale, and thus spare us the trouble
                // with incorrect locale settings.
                resetLocale();

                setupParallelism(argc, argv);
                printStartupMessage();
                const bool ok = setupParameters(argc, argv);
                if (!ok) {
                    return EXIT_FAILURE;
                }

                setupOutput();
                setupEbosSimulator();
                setupLogging();
                extractMessages();
                setupGridAndProps();
                runDiagnostics();
                setupState();
                writeInit();
                setupOutputWriter();
                setupLinearSolver();
                createSimulator();

                // Run.
                auto ret =  runSimulator();

                mergeParallelLogFiles();

                return ret;
            }
            catch (const std::exception &e) {
                std::ostringstream message;
                message  << "Program threw an exception: " << e.what();

                if( output_cout_ )
                {
                    // in some cases exceptions are thrown before the logging system is set
                    // up.
                    if (OpmLog::hasBackend("STREAMLOG")) {
                        OpmLog::error(message.str());
                    }
                    else {
                        std::cout << message.str() << "\n";
                    }
                }

                return EXIT_FAILURE;
            }
        }

    protected:
        void setupParallelism(int argc, char** argv)
        {
            // MPI setup.
            // Must ensure an instance of the helper is created to initialise MPI.
            // For a build without MPI the Dune::FakeMPIHelper is used, so rank will
            // be 0 and size 1.
            const Dune::MPIHelper& mpi_helper = Dune::MPIHelper::instance(argc, argv);
            mpi_rank_ = mpi_helper.rank();
            const int mpi_size = mpi_helper.size();
            output_cout_ = ( mpi_rank_ == 0 );
            must_distribute_ = ( mpi_size > 1 );

#ifdef _OPENMP
            // OpenMP setup.
            if (!getenv("OMP_NUM_THREADS")) {
                // Default to at most 4 threads, regardless of
                // number of cores (unless ENV(OMP_NUM_THREADS) is defined)
                int num_cores = omp_get_num_procs();
                int num_threads = std::min(4, num_cores);
                omp_set_num_threads(num_threads);
            }
#pragma omp parallel
            if (omp_get_thread_num() == 0) {
                // omp_get_num_threads() only works as expected within a parallel region.
                const int num_omp_threads = omp_get_num_threads();
                if (mpi_size == 1) {
                    std::cout << "OpenMP using " << num_omp_threads << " threads." << std::endl;
                } else {
                    std::cout << "OpenMP using " << num_omp_threads << " threads on MPI rank " << mpi_rank_ << "." << std::endl;
                }
            }
#endif
        }

        // Print startup message if on output rank.
        void printStartupMessage()
        {
            if (output_cout_) {
                const int lineLen = 70;
                const std::string version = moduleVersionName();
                const std::string banner = "This is flow_ebos (version "+version+")";
                const std::string ewomsVersion = "(eWoms version: " + Ewoms::versionString() + ")";
                const int bannerPreLen = (lineLen - 2 - banner.size())/2;
                const int bannerPostLen = bannerPreLen + (lineLen - 2 - banner.size())%2;
                const int eVPreLen = (lineLen - 2 - ewomsVersion.size())/2;
                const int eVPostLen = eVPreLen + (lineLen - 2 - ewomsVersion.size())%2;
                std::cout << "**********************************************************************\n";
                std::cout << "*                                                                    *\n";
                std::cout << "*" << std::string(bannerPreLen, ' ') << banner << std::string(bannerPostLen, ' ') << "*\n";
                std::cout << "*" << std::string(eVPreLen, ' ') << ewomsVersion << std::string(eVPostLen, ' ') << "*\n";
                std::cout << "*                                                                    *\n";
                std::cout << "* Flow is a simulator for fully implicit three-phase black-oil flow, *\n";
                std::cout << "*            and is part of OPM. For more information see:           *\n";
                std::cout << "*                       http://opm-project.org                       *\n";
                std::cout << "*                                                                    *\n";
                std::cout << "**********************************************************************\n\n";
            }
        }

        // Read parameters, see if a deck was specified on the command line, and if
        // it was, insert it into parameters.
        // Writes to:
        //   param_
        // Returns true if ok, false if not.
        bool setupParameters(int argc, char** argv)
        {
            param_ = parameter::ParameterGroup(argc, argv, false, output_cout_);

            // See if a deck was specified on the command line.
            if (!param_.unhandledArguments().empty()) {
                if (param_.unhandledArguments().size() != 1) {
                    std::cerr << "You can only specify a single input deck on the command line.\n";
                    return false;
                } else {
                    const auto casename = this->simulationCaseName( param_.unhandledArguments()[ 0 ] );
                    param_.insertParameter("deck_filename", casename.string() );
                }
            }

            // We must have an input deck. Grid and props will be read from that.
            if (!param_.has("deck_filename")) {
                std::cerr << "This program must be run with an input deck.\n"
                    "Specify the deck filename either\n"
                    "    a) as a command line argument by itself\n"
                    "    b) as a command line parameter with the syntax deck_filename=<path to your deck>, or\n"
                    "    c) as a parameter in a parameter file (.param or .xml) passed to the program.\n";
                return false;
            }
            return true;
        }

        // Set output_to_files_ and set/create output dir. Write parameter file.
        // Writes to:
        //   output_to_files_
        //   output_dir_
        // Throws std::runtime_error if failed to create (if requested) output dir.
        void setupOutput()
        {
            // Write parameters used for later reference. (only if rank is zero)
            output_to_files_ = output_cout_ && param_.getDefault("output", true);
            // Always read output_dir as it will be set unconditionally later.
            // Not doing this might cause files to be created in the current
            // directory.
            output_dir_ =
                param_.getDefault("output_dir", std::string("."));

            if (output_to_files_) {
                // Create output directory if needed.
                boost::filesystem::path fpath(output_dir_);
                if (!is_directory(fpath)) {
                    try {
                        create_directories(fpath);
                    }
                    catch (...) {
                        OPM_THROW(std::runtime_error, "Creating directories failed: " << fpath);
                    }
                }
                // Write simulation parameters.
                param_.writeParam(output_dir_ + "/simulation.param");
            }
        }

        // Setup OpmLog backend with output_dir.
        void setupLogging()
        {
            std::string deck_filename = param_.get<std::string>("deck_filename");
            // create logFile
            using boost::filesystem::path;
            path fpath(deck_filename);
            std::string baseName;
            std::ostringstream debugFileStream;
            std::ostringstream logFileStream;

            if (boost::to_upper_copy(path(fpath.extension()).string()) == ".DATA") {
                baseName = path(fpath.stem()).string();
            } else {
                baseName = path(fpath.filename()).string();
            }
            if (param_.has("output_dir")) {
                logFileStream << output_dir_ << "/";
                debugFileStream << output_dir_ + "/";
            }

            logFileStream << baseName;
            debugFileStream << "." << baseName;

            if ( must_distribute_ && mpi_rank_ != 0 )
            {
                // Added rank to log file for non-zero ranks.
                // This prevents message loss.
                debugFileStream << "."<< mpi_rank_;
                // If the following file appears then there is a bug.
                logFileStream << "." << mpi_rank_;
            }
            logFileStream << ".PRT";
            debugFileStream << ".DEBUG";

            std::string debugFile = debugFileStream.str();
            logFile_ = logFileStream.str();

            std::shared_ptr<EclipsePRTLog> prtLog = std::make_shared<EclipsePRTLog>(logFile_ , Log::NoDebugMessageTypes, false, output_cout_);
            std::shared_ptr<StreamLog> streamLog = std::make_shared<StreamLog>(std::cout, Log::StdoutMessageTypes);
            OpmLog::addBackend( "ECLIPSEPRTLOG" , prtLog );
            OpmLog::addBackend( "STREAMLOG", streamLog);
            std::shared_ptr<StreamLog> debugLog = std::make_shared<EclipsePRTLog>(debugFile, Log::DefaultMessageTypes, false, output_cout_);
            OpmLog::addBackend( "DEBUGLOG" ,  debugLog);
            const auto& msgLimits = eclState().getSchedule().getMessageLimits();
            const std::map<int64_t, int> limits = {{Log::MessageType::Note, msgLimits.getCommentPrintLimit(0)},
                                                   {Log::MessageType::Info, msgLimits.getMessagePrintLimit(0)},
                                                   {Log::MessageType::Warning, msgLimits.getWarningPrintLimit(0)},
                                                   {Log::MessageType::Error, msgLimits.getErrorPrintLimit(0)},
                                                   {Log::MessageType::Problem, msgLimits.getProblemPrintLimit(0)},
                                                   {Log::MessageType::Bug, msgLimits.getBugPrintLimit(0)}};
            prtLog->setMessageLimiter(std::make_shared<MessageLimiter>());
            prtLog->setMessageFormatter(std::make_shared<SimpleMessageFormatter>(false));
            streamLog->setMessageLimiter(std::make_shared<MessageLimiter>(10, limits));
            streamLog->setMessageFormatter(std::make_shared<SimpleMessageFormatter>(true));

            // Read parameters.
            if ( output_cout_ )
            {
                OpmLog::debug("\n---------------    Reading parameters     ---------------\n");
            }
        }

        void mergeParallelLogFiles()
        {
            // force closing of all log files.
            OpmLog::removeAllBackends();

            if( mpi_rank_ != 0 || !must_distribute_ )
            {
                return;
            }

            namespace fs = boost::filesystem;
            fs::path output_path(".");
            if ( param_.has("output_dir") )
            {
                output_path = fs::path(output_dir_);
            }

            fs::path deck_filename(param_.get<std::string>("deck_filename"));

            std::for_each(fs::directory_iterator(output_path),
                          fs::directory_iterator(),
                          detail::ParallelFileMerger(output_path, deck_filename.stem().string()));
        }

        void setupEbosSimulator()
        {
            std::string progName("flow_ebos");
            std::string deckFile("--ecl-deck-file-name=");
            deckFile += param_.get<std::string>("deck_filename");
            char* ptr[2];
            ptr[ 0 ] = const_cast< char * > (progName.c_str());
            ptr[ 1 ] = const_cast< char * > (deckFile.c_str());
            EbosSimulator::registerParameters();
            Ewoms::setupParameters_< TypeTag > ( 2, ptr );
            ebosSimulator_.reset(new EbosSimulator(/*verbose=*/false));
            ebosSimulator_->model().applyInitialSolution();

            try {
                if (output_cout_) {
                    MissingFeatures::checkKeywords(deck());
                }

                IOConfig& ioConfig = eclState().getIOConfig();
                ioConfig.setOutputDir(output_dir_);

                // Possible to force initialization only behavior (NOSIM).
                if (param_.has("nosim")) {
                    const bool nosim = param_.get<bool>("nosim");
                    ioConfig.overrideNOSIM( nosim );
                }
            }
            catch (const std::invalid_argument& e) {
                std::cerr << "Failed to create valid EclipseState object. See logfile: " << logFile_ << std::endl;
                std::cerr << "Exception caught: " << e.what() << std::endl;
                throw;
            }

            // Possibly override IOConfig setting (from deck) for how often RESTART files should get written to disk (every N report step)
            if (param_.has("output_interval")) {
                const int output_interval = param_.get<int>("output_interval");
                eclState().getRestartConfig().overrideRestartWriteInterval( size_t( output_interval ) );
            }
        }

        // Create grid and property objects.
        // Writes to:
        //   material_law_manager_
        //   fluidprops_
        //   gravity_
        void setupGridAndProps()
        {
            Dune::CpGrid& grid = ebosSimulator_->gridManager().grid();
            material_law_manager_ = ebosSimulator_->problem().materialLawManager();

            // create the legacy properties objects
            fluidprops_.reset(new BlackoilPropsAdFromDeck(deck(),
                                                          eclState(),
                                                          material_law_manager_,
                                                          grid));

            // Gravity.
            static_assert(Grid::dimension == 3, "Only 3D grids are supported");
            gravity_.fill(0.0);
            if (!deck().hasKeyword("NOGRAV"))
                gravity_[2] =
                    param_.getDefault("gravity", unit::gravity);

            // Geological properties
            bool use_local_perm = param_.getDefault("use_local_perm", true);
            geoprops_.reset(new DerivedGeology(grid, *fluidprops_, eclState(), use_local_perm, gravity_.data()));
        }

        const Deck& deck() const
        { return ebosSimulator_->gridManager().deck(); }

        Deck& deck()
        { return ebosSimulator_->gridManager().deck(); }

        const EclipseState& eclState() const
        { return ebosSimulator_->gridManager().eclState(); }

        EclipseState& eclState()
        { return ebosSimulator_->gridManager().eclState(); }

        // Initialise the reservoir state. Updated fluid props for SWATINIT.
        // Writes to:
        //   state_
        //   threshold_pressures_
        //   fluidprops_ (if SWATINIT is used)
        void setupState()
        {
            const PhaseUsage pu = Opm::phaseUsageFromDeck(deck());
            const Grid& grid = this->grid();

            // Need old-style fluid object for init purposes (only).
            BlackoilPropertiesFromDeck props(deck(),
                                             eclState(),
                                             material_law_manager_,
                                             grid.size(/*codim=*/0),
                                             grid.globalCell().data(),
                                             grid.logicalCartesianSize().data(),
                                             param_);


            // Init state variables (saturation and pressure).
            if (param_.has("init_saturation")) {
                state_.reset(new ReservoirState(grid.size(/*codim=*/0),
                                                grid.numFaces(),
                                                props.numPhases()));

                initStateBasic(grid.size(/*codim=*/0),
                               grid.globalCell().data(),
                               grid.logicalCartesianSize().data(),
                               grid.numFaces(),
                               Opm::UgGridHelpers::faceCells(grid),
                               Opm::UgGridHelpers::beginFaceCentroids(grid),
                               Opm::UgGridHelpers::beginCellCentroids(grid),
                               Grid::dimension,
                               props, param_, gravity_[2], *state_);

                initBlackoilSurfvol(Opm::UgGridHelpers::numCells(grid), props, *state_);

                enum { Oil = BlackoilPhases::Liquid, Gas = BlackoilPhases::Vapour };
                if (pu.phase_used[Oil] && pu.phase_used[Gas]) {
                    const int numPhases = props.numPhases();
                    const int numCells  = Opm::UgGridHelpers::numCells(grid);

                    // Uglyness 1: The state is a templated type, here we however make explicit use BlackoilState.
                    auto& gor = state_->getCellData( BlackoilState::GASOILRATIO );
                    const auto& surface_vol = state_->getCellData( BlackoilState::SURFACEVOL );
                    for (int c = 0; c < numCells; ++c) {
                        // Uglyness 2: Here we explicitly use the layout of the saturation in the surface_vol field.
                        gor[c] = surface_vol[ c * numPhases + pu.phase_pos[Gas]] / surface_vol[ c * numPhases + pu.phase_pos[Oil]];
                    }
                }
            } else if (deck().hasKeyword("EQUIL")) {
                // Which state class are we really using - what a f... mess?
                state_.reset( new ReservoirState( Opm::UgGridHelpers::numCells(grid),
                                                  Opm::UgGridHelpers::numFaces(grid),
                                                  props.numPhases()));

                initStateEquil(grid, props, deck(), eclState(), gravity_[2], *state_);
                //state_.faceflux().resize(Opm::UgGridHelpers::numFaces(grid), 0.0);
            } else {
                state_.reset( new ReservoirState( Opm::UgGridHelpers::numCells(grid),
                                                  Opm::UgGridHelpers::numFaces(grid),
                                                  props.numPhases()));
                initBlackoilStateFromDeck(Opm::UgGridHelpers::numCells(grid),
                                          Opm::UgGridHelpers::globalCell(grid),
                                          Opm::UgGridHelpers::numFaces(grid),
                                          Opm::UgGridHelpers::faceCells(grid),
                                          Opm::UgGridHelpers::beginFaceCentroids(grid),
                                          Opm::UgGridHelpers::beginCellCentroids(grid),
                                          Opm::UgGridHelpers::dimensions(grid),
                                          props, deck(), gravity_[2], *state_);
            }

            // The capillary pressure is scaled in fluidprops_ to match the scaled capillary pressure in props.
            if (deck().hasKeyword("SWATINIT")) {
                const int numCells = Opm::UgGridHelpers::numCells(grid);
                std::vector<int> cells(numCells);
                for (int c = 0; c < numCells; ++c) { cells[c] = c; }
                std::vector<double> pc = state_->saturation();
                props.capPress(numCells, state_->saturation().data(), cells.data(), pc.data(), nullptr);
                fluidprops_->setSwatInitScaling(state_->saturation(), pc);
            }
            initHydroCarbonState(*state_, pu, Opm::UgGridHelpers::numCells(grid), deck().hasKeyword("DISGAS"), deck().hasKeyword("VAPOIL"));
        }

        // Extract messages from parser.
        // Writes to:
        //    OpmLog singleton.
        void extractMessages()
        {
            if ( !output_cout_ )
            {
                return;
            }

            auto extractMessage = [this](const Message& msg) {
                auto log_type = this->convertMessageType(msg.mtype);
                const auto& location = msg.location;
                if (location) {
                    OpmLog::addMessage(log_type, Log::fileMessage(location.filename, location.lineno, msg.message));
                } else {
                    OpmLog::addMessage(log_type, msg.message);
                }
            };

            // Extract messages from Deck.
            for(const auto& msg : deck().getMessageContainer()) {
                extractMessage(msg);
            }

            // Extract messages from EclipseState.
            for (const auto& msg : eclState().getMessageContainer()) {
                extractMessage(msg);
            }
        }

        // Run diagnostics.
        // Writes to:
        //   OpmLog singleton.
        void runDiagnostics()
        {
            if( ! output_cout_ )
            {
                return;
            }

            // Run relperm diagnostics
            RelpermDiagnostics diagnostic;
            diagnostic.diagnosis(eclState(), deck(), this->grid());
        }

        void writeInit()
        {
            bool output      = param_.getDefault("output", true);
            bool output_ecl  = param_.getDefault("output_ecl", true);
            const Grid& grid = this->grid();
            if( output && output_ecl && output_cout_)
            {
                const EclipseGrid& inputGrid = eclState().getInputGrid();
                eclipse_writer_.reset(new EclipseWriter(eclState(), UgGridHelpers::createEclipseGrid( grid , inputGrid )));
                eclipse_writer_->writeInitial(geoprops_->simProps(grid),
                                              geoprops_->nonCartesianConnections());
            }
        }

        // Setup output writer.
        // Writes to:
        //   output_writer_
        void setupOutputWriter()
        {
            // create output writer after grid is distributed, otherwise the parallel output
            // won't work correctly since we need to create a mapping from the distributed to
            // the global view
            output_writer_.reset(new OutputWriter(grid(),
                                                  param_,
                                                  eclState(),
                                                  std::move(eclipse_writer_),
                                                  Opm::phaseUsageFromDeck(deck()),
                                                  fluidprops_->permeability()));
        }

        // Run the simulator.
        // Returns EXIT_SUCCESS if it does not throw.
        int runSimulator()
        {
            const auto& schedule = eclState().getSchedule();
            const auto& timeMap = schedule.getTimeMap();
            auto& ioConfig = eclState().getIOConfig();
            SimulatorTimer simtimer;

            // initialize variables
            const auto& initConfig = eclState().getInitConfig();
            simtimer.init(timeMap, (size_t)initConfig.getRestartStep());

            if (!ioConfig.initOnly()) {
                if (output_cout_) {
                    std::string msg;
                    msg = "\n\n================ Starting main simulation loop ===============\n";
                    OpmLog::info(msg);
                }

                SimulatorReport fullReport = simulator_->run(simtimer, *state_);

                if (output_cout_) {
                    std::ostringstream ss;
                    ss << "\n\n================    End of simulation     ===============\n\n";
                    fullReport.reportFullyImplicit(ss);
                    OpmLog::info(ss.str());
                    if (param_.anyUnused()) {
                        // This allows a user to catch typos and misunderstandings in the
                        // use of simulator parameters.
                        std::cout << "--------------------   Unused parameters:   --------------------\n";
                        param_.displayUsage();
                        std::cout << "----------------------------------------------------------------" << std::endl;
                    }
                }

                if (output_to_files_) {
                    std::string filename = output_dir_ + "/walltime.txt";
                    std::fstream tot_os(filename.c_str(), std::fstream::trunc | std::fstream::out);
                    fullReport.reportParam(tot_os);
                }
            } else {
                if (output_cout_) {
                    std::cout << "\n\n================ Simulation turned off ===============\n" << std::flush;
                }

            }
            return EXIT_SUCCESS;
        }

        // Setup linear solver.
        // Writes to:
        //   fis_solver_
        void setupLinearSolver()
        {
            typedef typename BlackoilModelEbos :: ISTLSolverType ISTLSolverType;

            extractParallelGridInformationToISTL(grid(), parallel_information_);
            fis_solver_.reset( new ISTLSolverType( param_, parallel_information_ ) );
        }

        /// This is the main function of Flow.
        // Create simulator instance.
        // Writes to:
        //   simulator_
        void createSimulator()
        {
            // Create the simulator instance.
            simulator_.reset(new Simulator(*ebosSimulator_,
                                           param_,
                                           *geoprops_,
                                           *fluidprops_,
                                           *fis_solver_,
                                           gravity_.data(),
                                           FluidSystem::enableDissolvedGas(),
                                           FluidSystem::enableVaporizedOil(),
                                           eclState(),
                                           *output_writer_,
                                           defunctWellNames()));
        }

    private:
        boost::filesystem::path simulationCaseName( const std::string& casename ) {
            namespace fs = boost::filesystem;

            const auto exists = []( const fs::path& f ) -> bool {
                if( !fs::exists( f ) ) return false;

                if( fs::is_regular_file( f ) ) return true;

                return fs::is_symlink( f )
                && fs::is_regular_file( fs::read_symlink( f ) );
            };

            auto simcase = fs::path( casename );

            if( exists( simcase ) ) {
                return simcase;
            }

            for( const auto& ext : { std::string("data"), std::string("DATA") } ) {
                if( exists( simcase.replace_extension( ext ) ) ) {
                    return simcase;
                }
            }

            throw std::invalid_argument( "Cannot find input case " + casename );
        }


        int64_t convertMessageType(const Message::type& mtype)
        {
            switch (mtype) {
            case Message::type::Debug:
                return Log::MessageType::Debug;
            case Message::type::Info:
                return Log::MessageType::Info;
            case Message::type::Warning:
                return Log::MessageType::Warning;
            case Message::type::Error:
                return Log::MessageType::Error;
            case Message::type::Problem:
                return Log::MessageType::Problem;
            case Message::type::Bug:
                return Log::MessageType::Bug;
            case Message::type::Note:
                return Log::MessageType::Note;
            }
            throw std::logic_error("Invalid messages type!\n");
        }

        Grid& grid()
        { return ebosSimulator_->gridManager().grid(); }

        std::unordered_set<std::string> defunctWellNames() const
        { return ebosSimulator_->gridManager().defunctWellNames(); }

        std::unique_ptr<EbosSimulator> ebosSimulator_;
        int  mpi_rank_ = 0;
        bool output_cout_ = false;
        bool must_distribute_ = false;
        parameter::ParameterGroup param_;
        bool output_to_files_ = false;
        std::string output_dir_ = std::string(".");
        std::shared_ptr<MaterialLawManager> material_law_manager_;
        std::unique_ptr<BlackoilPropsAdFromDeck> fluidprops_;
        std::array<double, 3> gravity_;
        std::unique_ptr<DerivedGeology> geoprops_;
        std::unique_ptr<ReservoirState> state_;
        std::unique_ptr<EclipseWriter> eclipse_writer_;
        std::unique_ptr<OutputWriter> output_writer_;
        boost::any parallel_information_;
        std::unique_ptr<NewtonIterationBlackoilInterface> fis_solver_;
        std::unique_ptr<Simulator> simulator_;
        std::string logFile_;
    };
} // namespace Opm

#endif // OPM_FLOW_MAIN_EBOS_HEADER_INCLUDED
