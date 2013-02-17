/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2009,2010,2011,2012, by the GROMACS development team, led by
 * David van der Spoel, Berk Hess, Erik Lindahl, and including many
 * others, as listed in the AUTHORS file in the top-level source
 * directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 * \brief
 * Implements gmx::analysismodules::Select.
 *
 * \author Teemu Murtola <teemu.murtola@gmail.com>
 * \ingroup module_trajectoryanalysis
 */
#include "select.h"

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <string>
#include <vector>

#include "gromacs/legacyheaders/gmxfio.h"
#include "gromacs/legacyheaders/smalloc.h"
#include "gromacs/legacyheaders/statutil.h"

#include "gromacs/analysisdata/analysisdata.h"
#include "gromacs/analysisdata/dataframe.h"
#include "gromacs/analysisdata/datamodule.h"
#include "gromacs/analysisdata/modules/average.h"
#include "gromacs/analysisdata/modules/plot.h"
#include "gromacs/options/basicoptions.h"
#include "gromacs/options/filenameoption.h"
#include "gromacs/options/options.h"
#include "gromacs/selection/selection.h"
#include "gromacs/selection/selectionoption.h"
#include "gromacs/trajectoryanalysis/analysissettings.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/stringutil.h"

namespace gmx
{

namespace analysismodules
{

namespace
{

/*! \internal \brief
 * Data module for writing index files.
 *
 * \ingroup module_analysisdata
 */
class IndexFileWriterModule : public AnalysisDataModuleInterface
{
    public:
        IndexFileWriterModule();
        virtual ~IndexFileWriterModule();

        //! Sets the file name to write the index file to.
        void setFileName(const std::string &fnm);
        /*! \brief
         * Adds information about a group to be printed.
         *
         * Must be called for each group present in the input data.
         */
        void addGroup(const std::string &name, bool bDynamic);

        virtual int flags() const;

        virtual void dataStarted(AbstractAnalysisData *data);
        virtual void frameStarted(const AnalysisDataFrameHeader &header);
        virtual void pointsAdded(const AnalysisDataPointSetRef &points);
        virtual void frameFinished(const AnalysisDataFrameHeader &header);
        virtual void dataFinished();

    private:
        void closeFile();

        struct GroupInfo
        {
            GroupInfo(const std::string &name, bool bDynamic)
                : name(name), bDynamic(bDynamic)
            { }

            std::string         name;
            bool                bDynamic;
        };

        std::string             fnm_;
        std::vector<GroupInfo>  groups_;
        FILE                   *fp_;
        int                     currentGroup_;
        int                     currentSize_;
        bool                    bAnyWritten_;
};

/********************************************************************
 * IndexFileWriterModule
 */

IndexFileWriterModule::IndexFileWriterModule()
    : fp_(NULL), currentGroup_(-1), currentSize_(0), bAnyWritten_(false)
{
}


IndexFileWriterModule::~IndexFileWriterModule()
{
    closeFile();
}


void IndexFileWriterModule::closeFile()
{
    if (fp_ != NULL)
    {
        gmx_fio_fclose(fp_);
        fp_ = NULL;
    }
}


void IndexFileWriterModule::setFileName(const std::string &fnm)
{
    fnm_ = fnm;
}


void IndexFileWriterModule::addGroup(const std::string &name, bool bDynamic)
{
    std::string newName(name);
    std::replace(newName.begin(), newName.end(), ' ', '_');
    groups_.push_back(GroupInfo(newName, bDynamic));
}


int IndexFileWriterModule::flags() const
{
    return efAllowMulticolumn | efAllowMultipoint;
}


void IndexFileWriterModule::dataStarted(AbstractAnalysisData * /*data*/)
{
    if (!fnm_.empty())
    {
        fp_ = gmx_fio_fopen(fnm_.c_str(), "w");
    }
}


void IndexFileWriterModule::frameStarted(const AnalysisDataFrameHeader & /*header*/)
{
    bAnyWritten_  = false;
    currentGroup_ = -1;
}


void
IndexFileWriterModule::pointsAdded(const AnalysisDataPointSetRef &points)
{
    if (fp_ == NULL)
    {
        return;
    }
    bool bFirstFrame = (points.frameIndex() == 0);
    if (points.firstColumn() == 0)
    {
        ++currentGroup_;
        GMX_RELEASE_ASSERT(currentGroup_ < static_cast<int>(groups_.size()),
                           "Too few groups initialized");
        if (bFirstFrame || groups_[currentGroup_].bDynamic)
        {
            if (!bFirstFrame || currentGroup_ > 0)
            {
                std::fprintf(fp_, "\n\n");
            }
            std::string name = groups_[currentGroup_].name;
            if (groups_[currentGroup_].bDynamic)
            {
                name += formatString("_f%d_t%.3f", points.frameIndex(), points.x());
            }
            std::fprintf(fp_, "[ %s ]", name.c_str());
            bAnyWritten_ = true;
            currentSize_ = 0;
        }
    }
    else
    {
        if (bFirstFrame || groups_[currentGroup_].bDynamic)
        {
            if (currentSize_ % 15 == 0)
            {
                std::fprintf(fp_, "\n");
            }
            std::fprintf(fp_, "%4d ", static_cast<int>(points.y(0)));
            ++currentSize_;
        }
    }
}


void IndexFileWriterModule::frameFinished(const AnalysisDataFrameHeader & /*header*/)
{
}


void IndexFileWriterModule::dataFinished()
{
    if (fp_ != NULL)
    {
        std::fprintf(fp_, "\n");
    }
    closeFile();
}

}       // namespace


/********************************************************************
 * Select
 */

const char Select::name[]             = "select";
const char Select::shortDescription[] =
    "Print general information about selections";

Select::Select()
    : TrajectoryAnalysisModule(name, shortDescription),
      selOpt_(NULL),
      bDump_(false), bTotNorm_(false), bFracNorm_(false), bResInd_(false),
      top_(NULL), occupancyModule_(new AnalysisDataAverageModule())
{
    registerAnalysisDataset(&sdata_, "size");
    registerAnalysisDataset(&cdata_, "cfrac");
    idata_.setColumnCount(2);
    idata_.setMultipoint(true);
    registerAnalysisDataset(&idata_, "index");
    registerAnalysisDataset(&mdata_, "mask");
    occupancyModule_->setXAxis(1.0, 1.0);
    registerBasicDataset(occupancyModule_.get(), "occupancy");
}


Select::~Select()
{
}


void
Select::initOptions(Options *options, TrajectoryAnalysisSettings * /*settings*/)
{
    static const char *const desc[] = {
        "[TT]g_select[tt] writes out basic data about dynamic selections.",
        "It can be used for some simple analyses, or the output can",
        "be combined with output from other programs and/or external",
        "analysis programs to calculate more complex things.",
        "Any combination of the output options is possible, but note",
        "that [TT]-om[tt] only operates on the first selection.",
        "Also note that if you provide no output options, no output is",
        "produced.[PAR]",
        "With [TT]-os[tt], calculates the number of positions in each",
        "selection for each frame. With [TT]-norm[tt], the output is",
        "between 0 and 1 and describes the fraction from the maximum",
        "number of positions (e.g., for selection 'resname RA and x < 5'",
        "the maximum number of positions is the number of atoms in",
        "RA residues). With [TT]-cfnorm[tt], the output is divided",
        "by the fraction covered by the selection.",
        "[TT]-norm[tt] and [TT]-cfnorm[tt] can be specified independently",
        "of one another.[PAR]",
        "With [TT]-oc[tt], the fraction covered by each selection is",
        "written out as a function of time.[PAR]",
        "With [TT]-oi[tt], the selected atoms/residues/molecules are",
        "written out as a function of time. In the output, the first",
        "column contains the frame time, the second contains the number",
        "of positions, followed by the atom/residue/molecule numbers.",
        "If more than one selection is specified, the size of the second",
        "group immediately follows the last number of the first group",
        "and so on. With [TT]-dump[tt], the frame time and the number",
        "of positions is omitted from the output. In this case, only one",
        "selection can be given.[PAR]",
        "With [TT]-on[tt], the selected atoms are written as a index file",
        "compatible with [TT]make_ndx[tt] and the analyzing tools. Each selection",
        "is written as a selection group and for dynamic selections a",
        "group is written for each frame.[PAR]",
        "For residue numbers, the output of [TT]-oi[tt] can be controlled",
        "with [TT]-resnr[tt]: [TT]number[tt] (default) prints the residue",
        "numbers as they appear in the input file, while [TT]index[tt] prints",
        "unique numbers assigned to the residues in the order they appear",
        "in the input file, starting with 1. The former is more intuitive,",
        "but if the input contains multiple residues with the same number,",
        "the output can be less useful.[PAR]",
        "With [TT]-om[tt], a mask is printed for the first selection",
        "as a function of time. Each line in the output corresponds to",
        "one frame, and contains either 0/1 for each atom/residue/molecule",
        "possibly selected. 1 stands for the atom/residue/molecule being",
        "selected for the current frame, 0 for not selected.",
        "With [TT]-dump[tt], the frame time is omitted from the output.[PAR]",
        "With [TT]-of[tt], the occupancy fraction of each position (i.e.,",
        "the fraction of frames where the position is selected) is",
        "printed.[PAR]",
        "With [TT]-ofpdb[tt], a PDB file is written out where the occupancy",
        "column is filled with the occupancy fraction of each atom in the",
        "selection. The coordinates in the PDB file will be those from the",
        "input topology. [TT]-pdbatoms[tt] can be used to control which atoms",
        "appear in the output PDB file: with [TT]all[tt] all atoms are",
        "present, with [TT]maxsel[tt] all atoms possibly selected by the",
        "selection are present, and with [TT]selected[tt] only atoms that are",
        "selected at least in one frame are present.[PAR]",
        "With [TT]-om[tt], [TT]-of[tt] and [TT]-ofpdb[tt], only one selection",
        "can be provided. [TT]-om[tt] and [TT]-of[tt] only accept dynamic",
        "selections."
    };

    options->setDescription(concatenateStrings(desc));

    options->addOption(FileNameOption("os").filetype(eftPlot).outputFile()
                           .store(&fnSize_).defaultBasename("size")
                           .description("Number of positions in each selection"));
    options->addOption(FileNameOption("oc").filetype(eftPlot).outputFile()
                           .store(&fnFrac_).defaultBasename("cfrac")
                           .description("Covered fraction for each selection"));
    options->addOption(FileNameOption("oi").filetype(eftGenericData).outputFile()
                           .store(&fnIndex_).defaultBasename("index")
                           .description("Indices selected by each selection"));
    options->addOption(FileNameOption("on").filetype(eftIndex).outputFile()
                           .store(&fnNdx_).defaultBasename("index")
                           .description("Index file from the selection"));
    options->addOption(FileNameOption("om").filetype(eftPlot).outputFile()
                           .store(&fnMask_).defaultBasename("mask")
                           .description("Mask for selected positions"));
    options->addOption(FileNameOption("of").filetype(eftPlot).outputFile()
                           .store(&fnOccupancy_).defaultBasename("occupancy")
                           .description("Occupied fraction for selected positions"));
    options->addOption(FileNameOption("ofpdb").filetype(eftPDB).outputFile()
                           .store(&fnPDB_).defaultBasename("occupancy")
                           .description("PDB file with occupied fraction for selected positions"));

    selOpt_ = options->addOption(SelectionOption("select").storeVector(&sel_)
                                     .required().multiValue()
                                     .description("Selections to analyze"));

    options->addOption(BooleanOption("dump").store(&bDump_)
                           .description("Do not print the frame time (-om, -oi) or the index size (-oi)"));
    options->addOption(BooleanOption("norm").store(&bTotNorm_)
                           .description("Normalize by total number of positions with -os"));
    options->addOption(BooleanOption("cfnorm").store(&bFracNorm_)
                           .description("Normalize by covered fraction with -os"));
    const char *const cResNumberEnum[] = { "number", "index", NULL };
    options->addOption(StringOption("resnr").store(&resNumberType_)
                           .enumValue(cResNumberEnum).defaultEnumIndex(0)
                           .description("Residue number output type with -oi and -on"));
    const char *const cPDBAtomsEnum[] = { "all", "maxsel", "selected", NULL };
    options->addOption(StringOption("pdbatoms").store(&pdbAtoms_)
                           .enumValue(cPDBAtomsEnum).defaultEnumIndex(0)
                           .description("Atoms to write with -ofpdb"));
}

void
Select::optionsFinished(Options                     * /*options*/,
                        TrajectoryAnalysisSettings *settings)
{
    if (!fnPDB_.empty())
    {
        settings->setFlag(TrajectoryAnalysisSettings::efRequireTop);
        settings->setFlag(TrajectoryAnalysisSettings::efUseTopX);
    }
    if ((!fnIndex_.empty() && bDump_)
        || !fnMask_.empty() || !fnOccupancy_.empty() || !fnPDB_.empty())
    {
        selOpt_->setValueCount(1);
    }
}

void
Select::initAnalysis(const TrajectoryAnalysisSettings &settings,
                     const TopologyInformation        &top)
{
    if (!sel_[0].isDynamic() && (!fnMask_.empty() || !fnOccupancy_.empty()))
    {
        GMX_THROW(InconsistentInputError(
                          "-om or -of are not meaningful with a static selection"));
    }
    bResInd_ = (resNumberType_ == "index");

    for (SelectionList::iterator i = sel_.begin(); i != sel_.end(); ++i)
    {
        i->initCoveredFraction(CFRAC_SOLIDANGLE);
    }

    // TODO: For large systems, a float may not have enough precision
    sdata_.setColumnCount(sel_.size());
    totsize_.reserve(sel_.size());
    for (size_t g = 0; g < sel_.size(); ++g)
    {
        totsize_.push_back(sel_[g].posCount());
    }
    if (!fnSize_.empty())
    {
        AnalysisDataPlotModulePointer plot(
                new AnalysisDataPlotModule(settings.plotSettings()));
        plot->setFileName(fnSize_);
        plot->setTitle("Selection size");
        plot->setXAxisIsTime();
        plot->setYLabel("Number");
        sdata_.addModule(plot);
    }

    cdata_.setColumnCount(sel_.size());
    if (!fnFrac_.empty())
    {
        AnalysisDataPlotModulePointer plot(
                new AnalysisDataPlotModule(settings.plotSettings()));
        plot->setFileName(fnFrac_);
        plot->setTitle("Covered fraction");
        plot->setXAxisIsTime();
        plot->setYLabel("Fraction");
        plot->setYFormat(6, 4);
        cdata_.addModule(plot);
    }

    // TODO: For large systems, a float may not have enough precision
    if (!fnIndex_.empty())
    {
        AnalysisDataPlotModulePointer plot(
                new AnalysisDataPlotModule(settings.plotSettings()));
        plot->setFileName(fnIndex_);
        plot->setPlainOutput(true);
        plot->setYFormat(4, 0);
        if (bDump_)
        {
            plot->setOmitX(bDump_);
            idata_.addColumnModule(1, 1, plot);
        }
        else
        {
            idata_.addModule(plot);
        }
    }
    if (!fnNdx_.empty())
    {
        boost::shared_ptr<IndexFileWriterModule> writer(new IndexFileWriterModule());
        writer->setFileName(fnNdx_);
        for (size_t g = 0; g < sel_.size(); ++g)
        {
            writer->addGroup(sel_[g].name(), sel_[g].isDynamic());
        }
        idata_.addModule(writer);
    }

    mdata_.setColumnCount(sel_[0].posCount());
    mdata_.addModule(occupancyModule_);
    if (!fnMask_.empty())
    {
        AnalysisDataPlotModulePointer plot(
                new AnalysisDataPlotModule(settings.plotSettings()));
        plot->setFileName(fnMask_);
        plot->setPlainOutput(bDump_);
        plot->setOmitX(bDump_);
        plot->setTitle("Selection mask");
        plot->setXAxisIsTime();
        plot->setYLabel("Occupancy");
        plot->setYFormat(1, 0);
        mdata_.addModule(plot);
    }
    if (!fnOccupancy_.empty())
    {
        AnalysisDataPlotModulePointer plot(
                new AnalysisDataPlotModule(settings.plotSettings()));
        plot->setFileName(fnOccupancy_);
        plot->setTitle("Fraction of time selection matches");
        plot->setXLabel("Selected position");
        plot->setYLabel("Occupied fraction");
        occupancyModule_->addColumnModule(0, 1, plot);
    }

    top_ = &top;
}


void
Select::analyzeFrame(int frnr, const t_trxframe &fr, t_pbc *pbc,
                     TrajectoryAnalysisModuleData *pdata)
{
    AnalysisDataHandle   sdh = pdata->dataHandle(sdata_);
    AnalysisDataHandle   cdh = pdata->dataHandle(cdata_);
    AnalysisDataHandle   idh = pdata->dataHandle(idata_);
    AnalysisDataHandle   mdh = pdata->dataHandle(mdata_);
    const SelectionList &sel = pdata->parallelSelections(sel_);
    t_topology          *top = top_->topology();

    sdh.startFrame(frnr, fr.time);
    for (size_t g = 0; g < sel.size(); ++g)
    {
        real normfac = bFracNorm_ ? 1.0 / sel[g].coveredFraction() : 1.0;
        if (bTotNorm_)
        {
            normfac /= totsize_[g];
        }
        sdh.setPoint(g, sel[g].posCount() * normfac);
    }
    sdh.finishFrame();

    cdh.startFrame(frnr, fr.time);
    for (size_t g = 0; g < sel.size(); ++g)
    {
        cdh.setPoint(g, sel[g].coveredFraction());
    }
    cdh.finishFrame();

    idh.startFrame(frnr, fr.time);
    for (size_t g = 0; g < sel.size(); ++g)
    {
        idh.setPoint(0, sel[g].posCount());
        idh.finishPointSet();
        for (int i = 0; i < sel[g].posCount(); ++i)
        {
            const SelectionPosition &p = sel[g].position(i);
            if (sel[g].type() == INDEX_RES && !bResInd_)
            {
                idh.setPoint(1, top->atoms.resinfo[p.mappedId()].nr);
            }
            else
            {
                idh.setPoint(1, p.mappedId() + 1);
            }
            idh.finishPointSet();
        }
    }
    idh.finishFrame();

    mdh.startFrame(frnr, fr.time);
    for (int i = 0; i < totsize_[0]; ++i)
    {
        mdh.setPoint(i, 0);
    }
    for (int i = 0; i < sel[0].posCount(); ++i)
    {
        mdh.setPoint(sel[0].position(i).refId(), 1);
    }
    mdh.finishFrame();
}


void
Select::finishAnalysis(int /*nframes*/)
{
}


void
Select::writeOutput()
{
    if (!fnPDB_.empty())
    {
        GMX_RELEASE_ASSERT(top_->hasTopology(),
                           "Topology should have been loaded or an error given earlier");
        t_atoms          atoms;
        atoms = top_->topology()->atoms;
        t_pdbinfo       *pdbinfo;
        snew(pdbinfo, atoms.nr);
        scoped_ptr_sfree pdbinfoGuard(pdbinfo);
        if (atoms.pdbinfo != NULL)
        {
            std::memcpy(pdbinfo, atoms.pdbinfo, atoms.nr*sizeof(*pdbinfo));
        }
        atoms.pdbinfo = pdbinfo;
        for (int i = 0; i < atoms.nr; ++i)
        {
            pdbinfo[i].occup = 0.0;
        }
        for (int i = 0; i < sel_[0].posCount(); ++i)
        {
            ConstArrayRef<int>                 atomIndices = sel_[0].position(i).atomIndices();
            ConstArrayRef<int>::const_iterator ai;
            for (ai = atomIndices.begin(); ai != atomIndices.end(); ++ai)
            {
                pdbinfo[*ai].occup = occupancyModule_->average(i);
            }
        }

        t_trxframe fr;
        clear_trxframe(&fr, TRUE);
        fr.bAtoms = TRUE;
        fr.atoms  = &atoms;
        fr.bX     = TRUE;
        fr.bBox   = TRUE;
        top_->getTopologyConf(&fr.x, fr.box);

        if (pdbAtoms_ == "all")
        {
            t_trxstatus *status = open_trx(fnPDB_.c_str(), "w");
            write_trxframe(status, &fr, NULL);
            close_trx(status);
        }
        else if (pdbAtoms_ == "maxsel")
        {
            ConstArrayRef<int> atomIndices = sel_[0].atomIndices();
            t_trxstatus       *status      = open_trx(fnPDB_.c_str(), "w");
            write_trxframe_indexed(status, &fr, atomIndices.size(),
                                   atomIndices.data(), NULL);
            close_trx(status);
        }
        else if (pdbAtoms_ == "selected")
        {
            std::vector<int> indices;
            for (int i = 0; i < sel_[0].posCount(); ++i)
            {
                if (occupancyModule_->average(i) > 0)
                {
                    ConstArrayRef<int>                 atomIndices = sel_[0].position(i).atomIndices();
                    ConstArrayRef<int>::const_iterator ai;
                    for (ai = atomIndices.begin(); ai != atomIndices.end(); ++ai)
                    {
                        indices.push_back(*ai);
                    }
                }
            }
            t_trxstatus *status = open_trx(fnPDB_.c_str(), "w");
            write_trxframe_indexed(status, &fr, indices.size(), &indices[0], NULL);
            close_trx(status);
        }
        else
        {
            GMX_RELEASE_ASSERT(false,
                               "Mismatch between -pdbatoms enum values and implementation");
        }
    }
}

} // namespace analysismodules

} // namespace gmx
