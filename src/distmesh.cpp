// --------------------------------------------------------------------
// This file is part of libDistMesh.
//
// libDistMesh is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// libDistMesh is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with libDistMesh.  If not, see <http://www.gnu.org/licenses/>.
//
// Copyright (C) 2015 Patrik Gebhardt
// Contact: patrik.gebhardt@rub.de
// --------------------------------------------------------------------

#include <vector>
#include <set>
#include <algorithm>

#include "distmesh/distmesh.h"
#include "distmesh/constants.h"
#include "distmesh/triangulation.h"

// easy creation of n-dimensional bounding box
Eigen::ArrayXXd distmesh::boundingBox(unsigned const dimension) {
    Eigen::ArrayXXd box(2, dimension);
    box.row(0).fill(-1.0);
    box.row(1).fill(1.0);
    return box;
}

// apply the distmesh algorithm
std::tuple<Eigen::ArrayXXd, Eigen::ArrayXXi> distmesh::distmesh(
    Functional const& distanceFunction, double const initialPointDistance,
    Functional const& elementSizeFunction, Eigen::Ref<Eigen::ArrayXXd const> const boundingBox,
    Eigen::Ref<Eigen::ArrayXXd const> const fixedPoints) {
    // determine dimension of mesh
    unsigned const dimension = boundingBox.cols();

    // create initial distribution in bounding box
    Eigen::ArrayXXd points = utils::createInitialPoints(distanceFunction,
        initialPointDistance, elementSizeFunction, boundingBox, fixedPoints);

    // create initial triangulation
    Eigen::ArrayXXi triangulation = triangulation::delaunay(points);

    // create buffer to store old point locations to calculate
    // retriangulation and stop criterion
    Eigen::ArrayXXd retriangulationCriterionBuffer = Eigen::ArrayXXd::Constant(
        points.rows(), points.cols(), INFINITY);
    Eigen::ArrayXXd stopCriterionBuffer = Eigen::ArrayXXd::Zero(
        points.rows(), points.cols());

    // main distmesh loop
    Eigen::ArrayXXi edgeIndices;
    for (unsigned step = 0; step < constants::maxSteps; ++step) {
        // retriangulate if point movement is above threshold
        if ((points - retriangulationCriterionBuffer).square().rowwise().sum().sqrt().maxCoeff() >
            constants::retriangulationThreshold * initialPointDistance) {
            // update triangulation
            triangulation = triangulation::delaunay(points);

            // reject triangles with circumcenter outside of the region
            Eigen::ArrayXXd circumcenter = Eigen::ArrayXXd::Zero(triangulation.rows(), dimension);
            for (int point = 0; point < triangulation.cols(); ++point) {
                circumcenter += utils::selectIndexedArrayElements<double>(
                    points, triangulation.col(point)) / triangulation.cols();
            }
            triangulation = utils::selectMaskedArrayElements<int>(triangulation,
                distanceFunction(circumcenter) < -constants::geometryEvaluationThreshold * initialPointDistance);

            // find unique edge indices
            edgeIndices = utils::findUniqueEdges(triangulation);

            // store current points positions
            retriangulationCriterionBuffer = points;
        }

        // calculate edge vectors and their length
        auto const edgeVector = (utils::selectIndexedArrayElements<double>(points, edgeIndices.col(0)) -
            utils::selectIndexedArrayElements<double>(points, edgeIndices.col(1))).eval();
        auto const edgeLength = edgeVector.square().rowwise().sum().sqrt().eval();

        // evaluate elementSizeFunction at midpoints of edges
        auto const desiredElementSize = elementSizeFunction(0.5 *
            (utils::selectIndexedArrayElements<double>(points, edgeIndices.col(0)) +
            utils::selectIndexedArrayElements<double>(points, edgeIndices.col(1)))).eval();

        // calculate desired edge length
        auto const desiredEdgeLength = (desiredElementSize * (1.0 + 0.4 / std::pow(2.0, dimension - 1)) *
            std::pow((edgeLength.pow(dimension).sum() / desiredElementSize.pow(dimension).sum()),
                1.0 / dimension)).eval();

        // calculate force vector for each edge
        auto const forceVector = (edgeVector.colwise() *
            ((desiredEdgeLength - edgeLength) / edgeLength).max(0.0)).eval();

        // store current points positions
        stopCriterionBuffer = points;

        // move points
        for (int edge = 0; edge < edgeIndices.rows(); ++edge) {
            if (edgeIndices(edge, 0) >= fixedPoints.rows()) {
                points.row(edgeIndices(edge, 0)) += constants::deltaT * forceVector.row(edge);
            }
            if (edgeIndices(edge, 1) >= fixedPoints.rows()) {
                points.row(edgeIndices(edge, 1)) -= constants::deltaT * forceVector.row(edge);
            }
        }

        // project points outside of domain to boundary
        utils::projectPointsToBoundary(distanceFunction, initialPointDistance, points);

        // stop, when maximum points movement is below threshold
        if ((points - stopCriterionBuffer).square().rowwise().sum().sqrt().maxCoeff() <
            constants::pointsMovementThreshold * initialPointDistance) {
            break;
        }
    }
    
    return std::make_tuple(points, triangulation);
}

// determine boundary edges of given triangulation
Eigen::ArrayXi distmesh::boundEdges(Eigen::Ref<Eigen::ArrayXXd const> const nodes,
    Eigen::Ref<Eigen::ArrayXXi const> const triangulation,
    Eigen::Ref<Eigen::ArrayXXi const> const externalEdges) {
    // create a new edge list, if none was given
    Eigen::ArrayXXi edges;
    if (externalEdges.rows() == 0) {
        edges = utils::findUniqueEdges(triangulation);
    }
    else {
        edges = externalEdges;
    }
    
    // get edge indices for each triangle in triangulation
    auto const edgeIndices = utils::getTriangulationEdgeIndices(triangulation, edges);
    
    // find edges, which only appear once in triangulation
    std::set<int> uniqueEdges;
    std::vector<int> boundaryEdges;
    for (int triangle = 0; triangle < triangulation.rows(); ++triangle)
    for (int edge = 0; edge < triangulation.cols(); ++edge) {
        auto const edgeIndex = edgeIndices(triangle, edge);
            
        // insert edge in set to get info about multiple appearance
        if (!std::get<1>(uniqueEdges.insert(edgeIndex))) {
            // find edge in vector and delete it
            auto const it = std::find(boundaryEdges.begin(), boundaryEdges.end(), edgeIndex);
            if (it != boundaryEdges.end()) {
                boundaryEdges.erase(it);
            }
        }
        else {
            boundaryEdges.push_back(edgeIndex);
        }
    }

    // convert stl vector to eigen array
    Eigen::ArrayXi boundary(boundaryEdges.size());
    for (int edge = 0; edge < boundary.rows(); ++edge) {
        boundary(edge) = boundaryEdges[edge];
    }
    
    // for the 2-D case fix orientation of boundary edges
    if (nodes.cols() == 2) {
        for (int edge = 0; edge < boundary.rows(); ++edge) {
            // find get index of element containing boundary edge
            int elementIndex = 0, edgeIndex = 0;
            (edgeIndices - boundary(edge)).square().minCoeff(&elementIndex, &edgeIndex);
            
            // get index of node not used in edge, but in the triangle
            int nodeIndex = 0;
            for (int node = 0; node < triangulation.cols(); ++node) {
                if ((triangulation(elementIndex, node) != edges(boundary(edge), 0)) &&
                    (triangulation(elementIndex, node) != edges(boundary(edge), 1))) {
                    nodeIndex = node;
                    break;
                }
            }
            
            // boundary edges with wrong orientation are marked with a negative sign
            auto const v1 = (nodes.row(edges(boundary(edge), 1)) - nodes.row(edges(boundary(edge), 0))).eval();
            auto const v2 = (nodes.row(triangulation(elementIndex, nodeIndex)) - nodes.row(edges(boundary(edge), 1))).eval();
            if (v1(0) * v2(1) - v1(1) * v2(0) < 0.0) {
                boundary(edge) *= -1;
            }
        }
    }
    
    return boundary;
}
