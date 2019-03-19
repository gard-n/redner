#include "edge.h"
#include "line_clip.h"
#include "scene.h"
#include "parallel.h"
#include "thrust_utils.h"
#include "ltc.inc"
#include "ltc_sphere.inc"

#include <thrust/iterator/constant_iterator.h>
#include <thrust/execution_policy.h>
#include <thrust/sort.h>
#include <thrust/transform_scan.h>
#include <thrust/binary_search.h>

// Set this to false to fallback to importance resampling if edge tree doesn't work
constexpr bool c_use_edge_tree = true;

struct edge_collector {
    DEVICE inline void operator()(int idx) {
        const auto &shape = *shape_ptr;
        // For each triangle
        auto ind = get_indices(shape, idx / 3);
        if ((idx % 3) == 0) {
            edges[idx] = Edge{shape_id,
                              min(ind[0], ind[1]),
                              max(ind[0], ind[1]),
                              idx / 3, -1};
        } else if ((idx % 3) == 1) {
            edges[idx] = Edge{shape_id,
                              min(ind[1], ind[2]),
                              max(ind[1], ind[2]),
                              idx / 3, -1};
        } else {
            edges[idx] = Edge{shape_id,
                              min(ind[2], ind[0]),
                              max(ind[2], ind[0]),
                              idx / 3, -1};
        }
    }

    int shape_id;
    const Shape *shape_ptr;
    Edge *edges;
};

struct edge_less_comparer {
    DEVICE inline bool operator()(const Edge &e0, const Edge &e1) {
        if (e0.v0 == e1.v0) {
            return e0.v1 < e1.v1;
        }
        return e0.v0 < e1.v0;
    }
};

struct edge_equal_comparer {
    DEVICE inline bool operator()(const Edge &e0, const Edge &e1) {
        return e0.v0 == e1.v0 && e0.v1 == e1.v1;
    }
};

struct edge_merger {
    DEVICE inline Edge operator()(const Edge &e0, const Edge &e1) {
        return Edge{e0.shape_id, e0.v0, e0.v1, e0.f0, e1.f0};
    }
};

struct primary_edge_weighter {
    DEVICE void operator()(int idx) {
        const auto &edge = edges[idx];
        auto &primary_edge_weight = primary_edge_weights[idx];
        auto v0 = get_v0(shapes, edge);
        auto v1 = get_v1(shapes, edge);
        auto v0p = Vector2{};
        auto v1p = Vector2{};
        primary_edge_weight = 0;
        // Project to screen space
        if (project(camera, Vector3(v0), Vector3(v1), v0p, v1p)) {
            auto v0c = v0p;
            auto v1c = v1p;
            // Clip against screen boundaries
            if (clip_line(v0p, v1p, v0c, v1c)) {
                // Reject non-silhouette edges
                auto org = xfm_point(camera.cam_to_world, Vector3{0, 0, 0});
                if (is_silhouette(shapes, org, edge)) {
                    primary_edge_weight = distance(v0c, v1c);
                }
            }
        }
    }

    Camera camera;
    const Shape *shapes;
    const Edge *edges;
    Real *primary_edge_weights;
};

struct secondary_edge_weighter {
    DEVICE void operator()(int idx) {
        const auto &edge = edges[idx];
        // We use the length * (pi - dihedral angle) to sample the edges
        // If the dihedral angle is large, it's less likely that the edge would be a silhouette
        auto &secondary_edge_weight = secondary_edge_weights[idx];
        auto exterior_dihedral = compute_exterior_dihedral_angle(shapes, edge);
        auto v0 = get_v0(shapes, edge);
        auto v1 = get_v1(shapes, edge);
        secondary_edge_weight = distance(v0, v1) * exterior_dihedral;
    }

    const Shape *shapes;
    const Edge *edges;
    Real *secondary_edge_weights;
};

EdgeSampler::EdgeSampler(const std::vector<const Shape*> &shapes,
                         const Scene &scene) {
    auto shapes_buffer = scene.shapes.view(0, shapes.size());
    // Conservatively allocate a big buffer for all edges
    auto num_total_triangles = 0;
    for (int shape_id = 0; shape_id < (int)shapes.size(); shape_id++) {
        num_total_triangles += shapes[shape_id]->num_triangles;
    }
    // Collect the edges
    // TODO: this assumes each edge is only associated with two triangles
    //       which may be untrue for some pathological meshes
    edges = Buffer<Edge>(scene.use_gpu, 3 * num_total_triangles);
    auto edges_buffer = Buffer<Edge>(scene.use_gpu, 3 * num_total_triangles);
    auto current_num_edges = 0;
    for (int shape_id = 0; shape_id < (int)shapes.size(); shape_id++) {
        parallel_for(edge_collector{
            shape_id,
            shapes_buffer.begin() + shape_id,
            edges.data + current_num_edges
        }, 3 * shapes[shape_id]->num_triangles, scene.use_gpu);
        // Merge the edges
        auto edges_begin = edges.data + current_num_edges;
        DISPATCH(scene.use_gpu, thrust::sort,
                 edges_begin,
                 edges_begin + 3 * shapes[shape_id]->num_triangles,
                 edge_less_comparer{});
        auto edges_buffer_begin = edges_buffer.data;
        auto new_end = DISPATCH(scene.use_gpu, thrust::reduce_by_key,
            edges_begin, // input keys
            edges_begin + 3 * shapes[shape_id]->num_triangles,
            edges_begin, // input values
            edges_buffer_begin, // output keys
            edges_buffer_begin, // output values
            edge_equal_comparer{},
            edge_merger{}).first;
        auto num_edges = new_end - edges_buffer_begin;
        DISPATCH(scene.use_gpu, thrust::copy, edges_buffer_begin, new_end, edges_begin);
        current_num_edges += num_edges;
    }
    edges.count = current_num_edges;
    // Primary edge sampler:
    primary_edges_pmf = Buffer<Real>(scene.use_gpu, edges.count);
    primary_edges_cdf = Buffer<Real>(scene.use_gpu, edges.count);
    // For each edge, if it is a silhouette, we project them on screen
    // and compute the screen-space length. We store the length in
    // primary_edges_pmf
    {
        parallel_for(primary_edge_weighter{
            scene.camera,
            scene.shapes.data,
            edges.begin(),
            primary_edges_pmf.begin()
        }, edges.size(), scene.use_gpu);
        // Compute PMF & CDF
        // First normalize primary_edges_pmf.
        auto total_length = DISPATCH(scene.use_gpu, thrust::reduce,
            primary_edges_pmf.begin(),
            primary_edges_pmf.end(),
            Real(0),
            thrust::plus<Real>());
        DISPATCH(scene.use_gpu, thrust::transform,
            primary_edges_pmf.begin(),
            primary_edges_pmf.end(),
            thrust::make_constant_iterator(total_length),
            primary_edges_pmf.begin(),
            thrust::divides<Real>());
        // Next we compute a prefix sum
        DISPATCH(scene.use_gpu, thrust::transform_exclusive_scan,
            primary_edges_pmf.begin(),
            primary_edges_pmf.end(),
            primary_edges_cdf.begin(),
            thrust::identity<Real>(), Real(0), thrust::plus<Real>());
    }

    // Secondary edge sampler
    if (!c_use_edge_tree) {
        // Build a global distribution if we are not using edge tree
        secondary_edges_pmf = Buffer<Real>(scene.use_gpu, edges.count);
        secondary_edges_cdf = Buffer<Real>(scene.use_gpu, edges.count);
        // For each edge we compute the length and store the length in 
        // secondary_edges_pmf
        parallel_for(secondary_edge_weighter{
            scene.shapes.data,
            edges.begin(),
            secondary_edges_pmf.begin()
        }, edges.size(), scene.use_gpu);
        {
            // Compute PMF & CDF
            // First normalize secondary_edges_pmf.
            auto total_length = DISPATCH(scene.use_gpu, thrust::reduce,
                secondary_edges_pmf.begin(),
                secondary_edges_pmf.end(),
                Real(0),
                thrust::plus<Real>());
            DISPATCH(scene.use_gpu, thrust::transform,
                secondary_edges_pmf.begin(),
                secondary_edges_pmf.end(),
                thrust::make_constant_iterator(total_length),
                secondary_edges_pmf.begin(),
                thrust::divides<Real>());
            // Next we compute a prefix sum
            DISPATCH(scene.use_gpu, thrust::transform_exclusive_scan,
                secondary_edges_pmf.begin(),
                secondary_edges_pmf.end(),
                secondary_edges_cdf.begin(),
                thrust::identity<Real>(), Real(0), thrust::plus<Real>());
        }
    } else {
        // Build a hierarchical data structure for edge sampling
        edge_tree = std::unique_ptr<EdgeTree>(
            new EdgeTree(scene.use_gpu,
                         scene.camera,
                         shapes_buffer,
                         edges.view(0, edges.size())));
    }
}

struct primary_edge_sampler {
    DEVICE void operator()(int idx) {
        // Initialize output
        edge_records[idx] = PrimaryEdgeRecord{};
        throughputs[2 * idx + 0] = Vector3{0, 0, 0};
        throughputs[2 * idx + 1] = Vector3{0, 0, 0};
        auto nd = channel_info.num_total_dimensions;
        for (int d = 0; d < nd; d++) {
            channel_multipliers[2 * nd * idx + d] = 0;
            channel_multipliers[2 * nd * idx + d + nd] = 0;
        }
        rays[2 * idx + 0] = Ray(Vector3{0, 0, 0}, Vector3{0, 0, 0});
        rays[2 * idx + 1] = Ray(Vector3{0, 0, 0}, Vector3{0, 0, 0});

        // Sample an edge by binary search on cdf
        auto sample = samples[idx];
        const Real *edge_ptr = thrust::upper_bound(thrust::seq,
                edges_cdf, edges_cdf + num_edges,
                sample.edge_sel);
        auto edge_id = clamp((int)(edge_ptr - edges_cdf - 1),
                                   0, num_edges - 1);
        const auto &edge = edges[edge_id];
        // Sample a point on the edge
        auto v0 = Vector3{get_v0(shapes, edge)};
        auto v1 = Vector3{get_v1(shapes, edge)};
        // Project the edge onto screen space
        auto v0_ss = Vector2{0, 0};
        auto v1_ss = Vector2{0, 0};
        if (!project(camera, v0, v1, v0_ss, v1_ss)) {
            return;
        }
        if (edges_pmf[edge_id] <= 0.f) {
            return;
        }

        if (!camera.fisheye) {
            // Uniform sample on the edge
            auto edge_pt = v0_ss + sample.t * (v1_ss - v0_ss);
            // Reject samples outside of image plane
            if (!in_screen(camera, edge_pt)) {
                return;
            }

            edge_records[idx].edge = edge;
            edge_records[idx].edge_pt = edge_pt;

            // Generate two rays at the two sides of the edge
            auto half_space_normal = get_normal(normalize(v0_ss - v1_ss));
            // The half space normal always points to the upper half-space.
            auto offset = 1e-6f;
            auto upper_pt = edge_pt + half_space_normal * offset;
            auto upper_ray = sample_primary(camera, upper_pt);
            auto lower_pt = edge_pt - half_space_normal * offset;
            auto lower_ray = sample_primary(camera, lower_pt);
            rays[2 * idx + 0] = upper_ray;
            rays[2 * idx + 1] = lower_ray;

            // Compute the corresponding backprop derivatives
            auto xi = clamp(int(edge_pt[0] * camera.width), 0, camera.width - 1);
            auto yi = clamp(int(edge_pt[1] * camera.height), 0, camera.height - 1);
            auto rd = channel_info.radiance_dimension;
            auto d_color = Vector3{
                d_rendered_image[nd * (yi * camera.width + xi) + rd + 0],
                d_rendered_image[nd * (yi * camera.width + xi) + rd + 1],
                d_rendered_image[nd * (yi * camera.width + xi) + rd + 2]
            };
            // The weight is the length of edge divided by the probability
            // of selecting this edge, divided by the length of gradients
            // of the edge equation w.r.t. screen coordinate.
            // For perspective projection the length of edge and gradients
            // cancel each other out.
            // For fisheye we need to compute the Jacobians
            auto upper_weight = d_color / edges_pmf[edge_id];
            auto lower_weight = -d_color / edges_pmf[edge_id];

            assert(isfinite(d_color));
            assert(isfinite(upper_weight));

            throughputs[2 * idx + 0] = upper_weight;
            throughputs[2 * idx + 1] = lower_weight;

            for (int d = 0; d < nd; d++) {
                auto d_channel = d_rendered_image[nd * (yi * camera.width + xi) + d];
                channel_multipliers[2 * nd * idx + d] = d_channel / edges_pmf[edge_id];
                channel_multipliers[2 * nd * idx + d + nd] = -d_channel / edges_pmf[edge_id];
            }
        } else {
            // In paper we focused on linear projection model.
            // However we also support non-linear models such as fisheye
            // projection.
            // To sample a point on the edge for non-linear models,
            // we need to sample in camera space instead of screen space,
            // since the edge is no longer a line segment in screen space.
            // Therefore we perform an "unprojection" to project the edge
            // from screen space to the film in camera space.
            // For perspective camera this is equivalent to sample in screen space:
            // we unproject (x, y) to (x', y', 1) where x', y' are just individual
            // affine transforms of x, y.
            // For fisheye camera we unproject from screen-space to the unit
            // sphere.
            // Therefore the following code also works for perspective camera,
            // but to make things more consistent to the paper we provide
            // two versions of code.
            auto v0_dir = screen_to_camera(camera, v0_ss);
            auto v1_dir = screen_to_camera(camera, v1_ss);
            // Uniform sample in camera space
            auto v_dir3 = v1_dir - v0_dir;
            auto edge_pt3 = v0_dir + sample.t * v_dir3;
            // Project back to screen space
            auto edge_pt = camera_to_screen(camera, edge_pt3);
            // Reject samples outside of image plane
            if (!in_screen(camera, edge_pt)) {
                // In theory this shouldn't happen since we clamp the edges
                return;
            }

            edge_records[idx].edge = edge;
            edge_records[idx].edge_pt = edge_pt;

            // The edge equation for the fisheye camera is:
            // alpha(p) = dot(p, cross(v0_dir, v1_dir))
            // Thus the half-space normal is cross(v0_dir, v1_dir)
            // Generate two rays at the two sides of the edge
            // We choose the ray offset such that the longer the edge is from
            // the camera, the smaller the offset is.
            auto half_space_normal = normalize(cross(v0_dir, v1_dir));
            auto v0_local = xfm_point(camera.world_to_cam, v0);
            auto v1_local = xfm_point(camera.world_to_cam, v1);
            auto edge_local = v0_local + sample.t * v1_local;
            auto offset = 1e-5f / length(edge_local);
            auto upper_dir = normalize(edge_pt3 + offset * half_space_normal);
            auto upper_pt = camera_to_screen(camera, upper_dir);
            auto upper_ray = sample_primary(camera, upper_pt);
            auto lower_dir = normalize(edge_pt3 - offset * half_space_normal);
            auto lower_pt = camera_to_screen(camera, lower_dir);
            auto lower_ray = sample_primary(camera, lower_pt);
            rays[2 * idx + 0] = upper_ray;
            rays[2 * idx + 1] = lower_ray;

            // Compute the corresponding backprop derivatives
            auto xi = int(edge_pt[0] * camera.width);
            auto yi = int(edge_pt[1] * camera.height);
            auto rd = channel_info.radiance_dimension;
            auto d_color = Vector3{
                d_rendered_image[nd * (yi * camera.width + xi) + rd + 0],
                d_rendered_image[nd * (yi * camera.width + xi) + rd + 1],
                d_rendered_image[nd * (yi * camera.width + xi) + rd + 2]
            };
            // The weight is the length of edge divided by the probability
            // of selecting this edge, divided by the length of gradients
            // of the edge equation w.r.t. screen coordinate.
            // For perspective projection the length of edge and gradients
            // cancel each other out.
            // For fisheye we need to compute the Jacobians
            auto upper_weight = d_color / edges_pmf[edge_id];
            auto lower_weight = -d_color / edges_pmf[edge_id];

            // alpha(p(x, y)) = dot(p(x, y), cross(v0_dir, v1_dir))
            // p = screen_to_camera(x, y)
            // dp/dx & dp/dy
            auto d_edge_dir_x = Vector3{0, 0, 0};
            auto d_edge_dir_y = Vector3{0, 0, 0};
            d_screen_to_camera(camera, edge_pt, d_edge_dir_x, d_edge_dir_y);
            // d alpha / d p = cross(v0_dir, v1_dir)
            auto d_alpha_dx = dot(d_edge_dir_x, cross(v0_dir, v1_dir));
            auto d_alpha_dy = dot(d_edge_dir_y, cross(v0_dir, v1_dir));
            auto dirac_jacobian = 1.f / sqrt(square(d_alpha_dx) + square(d_alpha_dy));
            // We use finite difference to compute the Jacobian
            // for sampling on the line
            auto jac_offset = Real(1e-6);
            auto edge_pt3_delta = v0_dir + (sample.t + jac_offset) * v_dir3;
            auto edge_pt_delta = camera_to_screen(camera, edge_pt3_delta);
            auto line_jacobian = length((edge_pt_delta - edge_pt) / offset);
            auto jacobian = line_jacobian * dirac_jacobian;
            upper_weight *= jacobian;
            lower_weight *= jacobian;

            assert(isfinite(upper_weight));

            throughputs[2 * idx + 0] = upper_weight;
            throughputs[2 * idx + 1] = lower_weight;
            for (int d = 0; d < nd; d++) {
                auto d_channel = d_rendered_image[nd * (yi * camera.width + xi) + d];
                channel_multipliers[2 * nd * idx + d] =
                    d_channel * jacobian / edges_pmf[edge_id];
                channel_multipliers[2 * nd * idx + d + nd] =
                    -d_channel * jacobian / edges_pmf[edge_id];
            }
        }

        // Ray differential computation
        auto screen_pos = edge_records[idx].edge_pt;
        auto ray = sample_primary(camera, screen_pos);
        auto delta = Real(1e-3);
        auto screen_pos_dx = screen_pos + Vector2{delta, Real(0)};
        auto ray_dx = sample_primary(camera, screen_pos_dx);
        auto screen_pos_dy = screen_pos + Vector2{Real(0), delta};
        auto ray_dy = sample_primary(camera, screen_pos_dy);
        auto pixel_size_x = Real(0.5) / camera.width;
        auto pixel_size_y = Real(0.5) / camera.height;
        auto org_dx = pixel_size_x * (ray_dx.org - ray.org) / delta;
        auto org_dy = pixel_size_y * (ray_dy.org - ray.org) / delta;
        auto dir_dx = pixel_size_x * (ray_dx.dir - ray.dir) / delta;
        auto dir_dy = pixel_size_y * (ray_dy.dir - ray.dir) / delta;
        primary_ray_differentials[idx] = RayDifferential{org_dx, org_dy, dir_dx, dir_dy};
    }

    const Camera camera;
    const Shape *shapes;
    const Edge *edges;
    int num_edges;
    const Real *edges_pmf;
    const Real *edges_cdf;
    const PrimaryEdgeSample *samples;
    const float *d_rendered_image;
    const ChannelInfo channel_info;
    PrimaryEdgeRecord *edge_records;
    Ray *rays;
    RayDifferential *primary_ray_differentials;
    Vector3 *throughputs;
    Real *channel_multipliers;
};

void sample_primary_edges(const Scene &scene,
                          const BufferView<PrimaryEdgeSample> &samples,
                          const float *d_rendered_image,
                          const ChannelInfo &channel_info,
                          BufferView<PrimaryEdgeRecord> edge_records,
                          BufferView<Ray> rays,
                          BufferView<RayDifferential> primary_ray_differentials,
                          BufferView<Vector3> throughputs,
                          BufferView<Real> channel_multipliers) {
    parallel_for(primary_edge_sampler{
        scene.camera,
        scene.shapes.data,
        scene.edge_sampler.edges.begin(),
        (int)scene.edge_sampler.edges.size(),
        scene.edge_sampler.primary_edges_pmf.begin(),
        scene.edge_sampler.primary_edges_cdf.begin(),
        samples.begin(),
        d_rendered_image,
        channel_info,
        edge_records.begin(),
        rays.begin(),
        primary_ray_differentials.begin(),
        throughputs.begin(),
        channel_multipliers.begin()
    }, samples.size(), scene.use_gpu);
}

struct primary_edge_weights_updater {
    DEVICE void operator()(int idx) {
        const auto &edge_record = edge_records[idx];
        auto isect_upper = shading_isects[2 * idx + 0];
        auto isect_lower = shading_isects[2 * idx + 1];
        auto &throughputs_upper = throughputs[2 * idx + 0];
        auto &throughputs_lower = throughputs[2 * idx + 1];
        // At least one of the intersections should be connected to the edge
        bool upper_connected = isect_upper.shape_id == edge_record.edge.shape_id &&
            (isect_upper.tri_id == edge_record.edge.f0 || isect_upper.tri_id == edge_record.edge.f1);
        bool lower_connected = isect_lower.shape_id == edge_record.edge.shape_id &&
            (isect_lower.tri_id == edge_record.edge.f0 || isect_lower.tri_id == edge_record.edge.f1);
        if (!upper_connected && !lower_connected) {
            throughputs_upper = Vector3{0, 0, 0};
            throughputs_lower = Vector3{0, 0, 0};
            auto nd = channel_info.num_total_dimensions;
            for (int d = 0; d < nd; d++) {
                channel_multipliers[2 * nd * idx + d] = 0;
                channel_multipliers[2 * nd * idx + d + nd] = 0;
            }
        }
    }

    const PrimaryEdgeRecord *edge_records;
    const Intersection *shading_isects;
    const ChannelInfo channel_info;
    Vector3 *throughputs;
    Real *channel_multipliers;
};

void update_primary_edge_weights(const Scene &scene,
                                 const BufferView<PrimaryEdgeRecord> &edge_records,
                                 const BufferView<Intersection> &edge_isects,
                                 const ChannelInfo &channel_info,
                                 BufferView<Vector3> throughputs,
                                 BufferView<Real> channel_multipliers) {
    // XXX: Disable this at the moment. Not sure if this is more robust or not.
    // parallel_for(primary_edge_weights_updater{
    //     edge_records.begin(),
    //     edge_isects.begin(),
    //     channel_info,
    //     throughputs.begin(),
    //     channel_multipliers.begin()
    // }, edge_records.size(), scene.use_gpu);
}

struct primary_edge_derivatives_computer {
    DEVICE void operator()(int idx) {
        const auto &edge_record = edge_records[idx];
        auto edge_contrib_upper = edge_contribs[2 * idx + 0];
        auto edge_contrib_lower = edge_contribs[2 * idx + 1];
        auto edge_contrib = edge_contrib_upper + edge_contrib_lower;

        auto &d_v0 = d_vertices[2 * idx + 0];
        auto &d_v1 = d_vertices[2 * idx + 1];
        auto &d_camera = d_cameras[idx];
        // Initialize derivatives
        d_v0 = DVertex{};
        d_v1 = DVertex{};
        d_camera = DCameraInst{};
        if (edge_record.edge.shape_id < 0) {
            return;
        }
        d_v0.shape_id = edge_record.edge.shape_id;
        d_v1.shape_id = edge_record.edge.shape_id;
        d_v0.vertex_id = edge_record.edge.v0;
        d_v1.vertex_id = edge_record.edge.v1;

        auto v0 = Vector3{get_v0(shapes, edge_record.edge)};
        auto v1 = Vector3{get_v1(shapes, edge_record.edge)};
        auto v0_ss = Vector2{0, 0};
        auto v1_ss = Vector2{0, 0};
        if (!project(camera, v0, v1, v0_ss, v1_ss)) {
            return;
        }
        auto d_v0_ss = Vector2{0, 0};
        auto d_v1_ss = Vector2{0, 0};
        auto edge_pt = edge_record.edge_pt;
        if (!camera.fisheye) {
            // Equation 8 in the paper
            d_v0_ss.x = v1_ss.y - edge_pt.y;
            d_v0_ss.y = edge_pt.x - v1_ss.x;
            d_v1_ss.x = edge_pt.y - v0_ss.y;
            d_v1_ss.y = v0_ss.x - edge_pt.x;
        } else {
            // This also works for perspective camera,
            // but for consistency we provide two versions.
            // alpha(p) = dot(p, cross(v0_dir, v1_dir))
            // v0_dir = screen_to_camera(v0_ss)
            // v1_dir = screen_to_camera(v1_ss)
            // d alpha / d v0_ss_x = dot(cross(v1_dir, p),
            //     d_screen_to_camera(v0_ss).x)
            auto v0_dir = screen_to_camera(camera, v0_ss);
            auto v1_dir = screen_to_camera(camera, v1_ss);
            auto edge_dir = screen_to_camera(camera, edge_pt);
            auto d_v0_dir_x = Vector3{0, 0, 0};
            auto d_v0_dir_y = Vector3{0, 0, 0};
            d_screen_to_camera(camera, v0_ss, d_v0_dir_x, d_v0_dir_y);
            auto d_v1_dir_x = Vector3{0, 0, 0};
            auto d_v1_dir_y = Vector3{0, 0, 0};
            d_screen_to_camera(camera, v1_ss, d_v1_dir_x, d_v1_dir_y);
            d_v0_ss.x = dot(cross(v1_dir, edge_dir), d_v0_dir_x);
            d_v0_ss.y = dot(cross(v1_dir, edge_dir), d_v0_dir_y);
            d_v1_ss.x = dot(cross(edge_dir, v0_dir), d_v1_dir_x);
            d_v1_ss.y = dot(cross(edge_dir, v0_dir), d_v1_dir_y);
        }
        d_v0_ss *= edge_contrib;
        d_v1_ss *= edge_contrib;

        // v0_ss, v1_ss = project(camera, v0, v1)
        d_project(camera, v0, v1,
            d_v0_ss.x, d_v0_ss.y,
            d_v1_ss.x, d_v1_ss.y,
            d_camera,
            d_v0.d_v, d_v1.d_v);
    }

    const Camera camera;
    const Shape *shapes;
    const PrimaryEdgeRecord *edge_records;
    const Real *edge_contribs;
    DVertex *d_vertices;
    DCameraInst *d_cameras;
};

void compute_primary_edge_derivatives(const Scene &scene,
                                      const BufferView<PrimaryEdgeRecord> &edge_records,
                                      const BufferView<Real> &edge_contribs,
                                      BufferView<DVertex> d_vertices,
                                      BufferView<DCameraInst> d_cameras) {
    parallel_for(primary_edge_derivatives_computer{
        scene.camera,
        scene.shapes.data,
        edge_records.begin(),
        edge_contribs.begin(),
        d_vertices.begin(), d_cameras.begin()
    }, edge_records.size(), scene.use_gpu);
}

DEVICE
inline Matrix3x3 get_ltc_matrix(const Material &material,
                                const SurfacePoint &surface_point,
                                const Vector3 &wi,
                                Real min_rough) {
    auto roughness = max(get_roughness(material, surface_point), min_rough);
    auto cos_theta = dot(wi, surface_point.shading_frame.n);
    auto theta = acos(cos_theta);
    // search lookup table
    auto rid = clamp(int(roughness * (ltc::size - 1)), 0, ltc::size - 1);
    auto tid = clamp(int((theta / (M_PI / 2.f)) * (ltc::size - 1)), 0, ltc::size - 1);
    // TODO: linear interpolation?
    return Matrix3x3(ltc::tabM[rid+tid*ltc::size]);
}

struct secondary_edge_sampler {
    DEVICE
    Real get_sphere_tab(Real cos_theta, Real form_factor) {
        auto N = ltc::tab_sphere_size;
        auto uid = clamp(int(cos_theta * 0.5f + 0.5f * (N - 1)), 0, N - 1);
        auto vid = clamp(int(form_factor * (N - 1)), 0, N - 1);
        return ltc::tabSphere[uid + vid * N];
    }

    DEVICE Vector3 solve_cubic(Real c0, Real c1, Real c2, Real c3) {
        // https://blog.selfshadow.com/ltc/webgl/ltc_disk.html
        // http://momentsingraphics.de/?p=105

        // Normalize the polynomial
        auto inv_c3 = 1.f / c3;
        c0 *= inv_c3;
        c1 *= inv_c3;
        c2 *= inv_c3;
        // Divide middle coefficients by three
        c1 /= 3.f;
        c2 /= 3.f;

        auto A = c3;
        auto B = c2;
        auto C = c1;
        auto D = c0;

        // Compute the Hessian and the discriminant
        auto delta = Vector3{
            -square(c2) + c1,
            -c1 * c2 + c0,
            dot(Vector2{c2, -c1}, Vector2{c0, c1})
        };

        auto discriminant = dot(
            Vector2{4.0f * delta.x, -delta.y},
            Vector2{delta.z, delta.y});

        auto xlc = Vector2{0, 0};
        auto xsc = Vector2{0, 0};

        // Algorithm A
        {
            // auto A_a = Real(1);
            auto C_a = delta.x;
            auto D_a = -2.0f * B * delta.x + delta.y;

            // Take the cubic root of a normalized complex number
            auto theta = atan2(sqrt(discriminant), -D_a) / 3.0f;

            auto x_1a = 2.0f * sqrt(-C_a) * cos(theta);
            auto x_3a = 2.0f * sqrt(-C_a) * cos(theta + (2.0/3.0) * Real(M_PI));

            auto xl = Real(0);
            if ((x_1a + x_3a) > 2.0f * B) {
                xl = x_1a;
            } else {
                xl = x_3a;
            }

            xlc = Vector2{xl - B, A};
        }

        // Algorithm D
        {
            // auto A_d = D;
            auto C_d = delta.z;
            auto D_d = -D * delta.y + 2.0 * C * delta.z;

            // Take the cubic root of a normalized complex number
            auto theta = atan2(D * sqrt(discriminant), -D_d) / 3.0f;

            auto x_1d = 2.0f * sqrt(-C_d) * cos(theta);
            auto x_3d = 2.0f * sqrt(-C_d) * cos(theta + (2.0f/3.0f)*Real(M_PI));

            auto xs = Real(0);
            if (x_1d + x_3d < 2.0f*C) {
                xs = x_1d;
            } else {
                xs = x_3d;
            }

            xsc = Vector2{-D, xs + C};
        }

        auto E =  xlc.y*xsc.y;
        auto F = -xlc.x*xsc.y - xlc.y*xsc.x;
        auto G =  xlc.x*xsc.x;

        auto xmc = Vector2{C*F - B*G, -B*F + C*E};

        auto root = Vector3{xsc.x/xsc.y, xmc.x/xmc.y, xlc.x/xlc.y};

        if (root.x < root.y && root.x < root.z) {
            root = Vector3{root.y, root.x, root.z};
        } else if (root.z < root.x && root.z < root.y) {
            root = Vector3{root.x, root.z, root.y};
        }

        return root;
    }

    DEVICE Real ltc_sphere_integral(const Sphere &b_sphere,
                                    const SurfacePoint &p,
                                    const Matrix3x3 &m_inv) {
        // TODO: there might be a faster way to do this for pure diffuse case
        // https://blog.selfshadow.com/ltc/webgl/ltc_disk.html
        // We integrate over the sphere by creating a disk having the same solid angle
        // then we apply m_inv to transform it
        // C = center of the disk
        auto C = to_local(p.shading_frame, b_sphere.center);
        // V1, V2 = coordinate frame
        auto V1 = Vector3{0, 0, 0}, V2 = Vector3{0, 0, 0};
        coordinate_system(C, V1, V2);
        V1 *= b_sphere.radius;
        V2 *= b_sphere.radius;
        C = m_inv * C;
        V1 = m_inv * V1;
        V2 = m_inv * V2;
        if (dot(cross(V1, V2), C) <= 0) {
            return 0;
        }
        // V1 & V2 are not orthogonal after transformation
        // Therefore we compute eigen decomposition
        auto a = Real(0);
        auto b = Real(0);
        auto d11 = dot(V1, V1);
        auto d22 = dot(V2, V2);
        auto d12 = dot(V1, V2);
        if (fabs(d12) / sqrt(d11 * d22) > 1e-4f) {
            auto tr = d11 + d22;
            auto det = -d12*d12 + d11*d22;

            // Use sqrt matrix to solve for eigenvalues
            det = sqrt(det);
            auto u = 0.5f * sqrt(tr - 2.0f * det);
            auto v = 0.5f * sqrt(tr + 2.0f * det);
            auto e_max = square(u + v);
            auto e_min = square(u - v);

            auto V1_ = Vector3{0, 0, 0};
            auto V2_ = Vector3{0, 0, 0};

            if (d11 > d22) {
                V1_ = d12*V1 + (e_max - d11)*V2;
                V2_ = d12*V1 + (e_min - d11)*V2;
            } else {
                V1_ = d12*V2 + (e_max - d22)*V1;
                V2_ = d12*V2 + (e_min - d22)*V1;
            }

            a = 1.f / e_max;
            b = 1.f / e_min;
            V1 = normalize(V1_);
            V2 = normalize(V2_);
        } else {
            a = 1.f / d11;
            b = 1.f / d22;
            V1 *= sqrt(a);
            V2 *= sqrt(b);
        }

        auto V3 = cross(V1, V2);
        if (dot(C, V3) < 0.f) {
            V3 = -V3;
        }

        auto L = dot(V3, C);
        auto x0 = dot(V1, C) / L;
        auto y0 = dot(V2, C) / L;

        a *= square(L);
        b *= square(L);

        // Find the sphere that has the same solid angle as the ellipse
        auto c0 = a * b;
        auto c1 = a * b * (1.f + square(x0) + square(y0)) - a - b;
        auto c2 = 1.f - a * (1.f + square(x0)) - b * (1.f + square(y0));
        auto c3 = Real(1);
        auto roots = solve_cubic(c0, c1, c2, c3);
        auto e1 = roots.x;
        auto e2 = roots.y;
        auto e3 = roots.z;
        auto avg_dir = Vector3{a*x0/(a - e2), b*y0/(b - e2), Real(1)};
        auto rotate = Matrix3x3{
            V1.x, V2.x, V3.x,
            V1.y, V2.y, V3.y,
            V1.z, V2.z, V3.z};
        avg_dir = normalize(rotate * avg_dir);
        auto L1 = sqrt(-e2 / e3);
        auto L2 = sqrt(-e2 / e1);
        auto form_factor = L1 * L2 / sqrt((1.f + square(L1)) * (1.f + square(L2)));
        assert(isfinite(form_factor));
        return get_sphere_tab(avg_dir.z, form_factor) * form_factor;
    }

    DEVICE bool is_bound_below_surface(const AABB3 &bounds,
                                       const SurfacePoint &p) {
        for (int i = 0; i < 8; i++) {
            auto c = corner(bounds, i);
            if (dot(p.shading_frame[2],
                    c - p.position) > 0.f) {
                return false;
            }
        }
        return true;
    }

    DEVICE Real importance(const BVHNode3 &node,
                           const SurfacePoint &p,
                           const Matrix3x3 &m_inv) {
        // If the node is below the surface point, the importance is 0
        if (is_bound_below_surface(node.bounds, p)) {
            return 0;
        }
        // importance = BRDF * weighted length / dist^2
        // For BRDF we take the bounding sphere of the AABB and integrate the LTC over it
        // (see https://blogs.unity3d.com/2017/07/21/real-time-line-and-disk-light-shading-with-linearly-transformed-cosines/)
        std::cerr << "node.bounds:" << node.bounds << std::endl;
        auto b_sphere = compute_bounding_sphere(node.bounds);
        auto brdf_term = Real(M_PI);
        if (!inside(b_sphere, p.position)) {
            brdf_term = ltc_sphere_integral(b_sphere, p, m_inv);
        }
        return brdf_term * node.weighted_total_length
            / max(distance_squared(b_sphere.center, p.position), Real(1e-6f));
    }

    DEVICE Real importance(const BVHNode6 &node,
                           const SurfacePoint &p,
                           const Matrix3x3 &m_inv) {
        // If the node is below the surface point, the importance is 0
        auto p_bounds = AABB3{node.bounds.p_min, node.bounds.p_max};
        if (is_bound_below_surface(p_bounds, p)) {
            return 0;
        }
        // importance = BRDF * weighted length / dist^2
        // Except if the sphere centered at 0.5 * (p + cam_org),
        // which has radius of 0.5 * distance(p, cam_org)
        // does not intersect the directional bounding box of node, 
        // the importance is zero (see Olson and Zhang 2006)
        auto d_bounds = AABB3{node.bounds.d_min, node.bounds.d_max};
        if (!intersect(Sphere{0.5f * (p.position + cam_org),
                              0.5f * distance(p.position, cam_org)}, d_bounds)) {
            return 0;
        }
        std::cerr << "p_bounds:" << p_bounds << std::endl;
        auto b_sphere = compute_bounding_sphere(p_bounds);
        auto brdf_term = Real(M_PI);
        if (!inside(b_sphere, p.position)) {
            brdf_term = ltc_sphere_integral(b_sphere, p, m_inv);
        }
        return brdf_term * node.weighted_total_length
            / max(distance_squared(b_sphere.center, p.position), Real(1e-6f));
    }

    template <typename BVHNodeType>
    DEVICE int sample_edge(const BVHNodeType &node,
                           const SurfacePoint &p,
                           const Matrix3x3 &m_inv,
                           Real sample,
                           Real &pmf) {
        if (node.edge_id != -1) {
            assert(node.children[0] == nullptr &&
                   node.children[1] == nullptr);
            return node.edge_id;
        }
        assert(node.children[0] != nullptr && node.children[1] != nullptr);
        auto imp0 = importance(*node.children[0], p, m_inv);
        auto imp1 = importance(*node.children[1], p, m_inv);
        if (imp0 <= 0 && imp1 <= 0) {
            return -1;
        }
        auto prob_0 = imp0 / (imp0 + imp1);
        if (sample < prob_0) {
            pmf *= prob_0;
            std::cerr << "pmf:" << pmf << std::endl;
            // Rescale to [0, 1]
            sample = sample * (imp0 + imp1) / imp0;
            return sample_edge(*node.children[0],
                               p,
                               m_inv,
                               sample,
                               pmf);
        } else {
            auto prob_1 = 1.f - prob_0;
            pmf *= prob_1;
            // Rescale to [0, 1]
            sample = (sample * (imp0 + imp1) - imp0) / imp1;
            return sample_edge(*node.children[1],
                               p,
                               m_inv,
                               sample,
                               pmf);
        }
    }

    DEVICE int sample_edge(const EdgeTreeRoots &edge_tree_roots,
                           const SurfacePoint &p,
                           const Matrix3x3 &m_inv,
                           Real sample,
                           Real &pmf) {
        auto imp_cs = Real(0);
        if (edge_tree_roots.cs_bvh_root != nullptr) {
            imp_cs = importance(*edge_tree_roots.cs_bvh_root,
                                p,
                                m_inv);
        }
        auto imp_ncs = Real(0);
        if (edge_tree_roots.ncs_bvh_root != nullptr) {
            imp_ncs = importance(*edge_tree_roots.ncs_bvh_root,
                                 p,
                                 m_inv);
        }
        if (imp_cs <= 0 && imp_ncs <= 0) {
            return -1;
        }
        auto prob_cs = imp_cs / (imp_cs + imp_ncs);
        std::cerr << "imp_cs:" << imp_cs << std::endl;
        std::cerr << "imp_ncs:" << imp_ncs << std::endl;
        std::cerr << "sample:" << sample << ", prob_cs:" << prob_cs << std::endl;
        if (sample < prob_cs) {
            pmf = prob_cs;
            // Rescale to [0, 1]
            sample = sample * (imp_cs + imp_ncs) / imp_cs;
            std::cerr << "sample:" << sample << std::endl;
            std::cerr << "pmf:" << pmf << std::endl;
            return sample_edge(*edge_tree_roots.cs_bvh_root,
                               p,
                               m_inv,
                               sample,
                               pmf);
        } else {
            pmf = 1.f - prob_cs;
            // Rescale to [0, 1]
            sample = (sample * (imp_cs + imp_ncs) - imp_cs) / imp_ncs;
            return sample_edge(*edge_tree_roots.ncs_bvh_root,
                               p,
                               m_inv,
                               sample,
                               pmf);
        }
    }

    DEVICE void operator()(int idx) {
        auto pixel_id = active_pixels[idx];
        const auto &edge_sample = edge_samples[idx];
        const auto &wi = -incoming_rays[pixel_id].dir;
        const auto &shading_isect = shading_isects[pixel_id];
        const auto &shading_point = shading_points[pixel_id];
        const auto &throughput = throughputs[pixel_id];
        const auto &min_rough = min_roughness[pixel_id];

        // Initialize output
        edge_records[idx] = SecondaryEdgeRecord{};
        new_throughputs[2 * idx + 0] = Vector3{0, 0, 0};
        new_throughputs[2 * idx + 1] = Vector3{0, 0, 0};
        rays[2 * idx + 0] = Ray(Vector3{0, 0, 0}, Vector3{0, 0, 0});
        rays[2 * idx + 1] = Ray(Vector3{0, 0, 0}, Vector3{0, 0, 0});
        edge_min_roughness[2 * idx + 0] = min_rough;
        edge_min_roughness[2 * idx + 1] = min_rough;

        // XXX Hack: don't compute secondary edge derivatives if we already hit a diffuse vertex
        // before shading_point.
        // Such paths are extremely noisy and have very small contribution to the actual derivatives.
        if (min_rough > 1e-2f) {
            return;
        }

        // Setup the Linearly Transformed Cosine Distribution
        const Shape &shape = shapes[shading_isect.shape_id];
        const Material &material = materials[shape.material_id];
        // First decide which component of BRDF to sample
        auto diffuse_reflectance = get_diffuse_reflectance(material, shading_point);
        auto specular_reflectance = get_specular_reflectance(material, shading_point);
        auto diffuse_weight = luminance(diffuse_reflectance);
        auto specular_weight = luminance(specular_reflectance);
        auto weight_sum = diffuse_weight + specular_weight;
        if (weight_sum <= 0.f) {
            // black material
            return;
        }
        auto diffuse_pmf = diffuse_weight / weight_sum;
        auto specular_pmf = specular_weight / weight_sum;
        auto m_pmf = 0.f;
        auto n = shading_point.shading_frame.n;
        auto frame_x = normalize(wi - n * dot(wi, n));
        auto frame_y = cross(n, frame_x);
        auto isotropic_frame = Frame{frame_x, frame_y, n};
        auto m = Matrix3x3{};
        auto m_inv = Matrix3x3{};
        auto ltc_magnitude = Real(0);
        if (edge_sample.bsdf_component <= diffuse_pmf) {
            // M is shading frame * identity
            m_inv = Matrix3x3(isotropic_frame);
            m = inverse(m_inv);
            m_pmf = diffuse_pmf;
            ltc_magnitude = Real(1);
        } else {
            m_inv = inverse(get_ltc_matrix(material, shading_point, wi, min_rough)) *
                    Matrix3x3(isotropic_frame);
            m = inverse(m_inv);
            m_pmf = specular_pmf;
        }

        std::cerr << "test" << std::endl;
        auto edge_id = -1;
        auto edge_sample_weight = Real(0);
        if (edges_pmf != nullptr) {
            // Sample an edge by importance resampling:
            // We randomly sample M edges, estimate contribution based on LTC, 
            // then sample based on the estimated contribution.
            constexpr int M = 64;
            int edge_ids[M];
            Real edge_weights[M];
            Real resample_cdf[M];
            for (int sample_id = 0; sample_id < M; sample_id++) {
                // Sample an edge by binary search on cdf
                // We use some form of stratification over the M samples here: 
                // the random number we use is mod(edge_sample.edge_sel + i / M, 1)
                // It enables us to choose M edges with a single random number
                const Real *edge_ptr = thrust::upper_bound(thrust::seq,
                    edges_cdf, edges_cdf + num_edges,
                    modulo(edge_sample.edge_sel + Real(sample_id) / M, Real(1)));
                auto edge_id = clamp((int)(edge_ptr - edges_cdf - 1), 0, num_edges - 1);
                edge_ids[sample_id] = edge_id;
                edge_weights[sample_id] = 0;
                const auto &edge = edges[edge_id];
                // If the edge lies on the same triangle of shading isects, the weight is 0
                // If not a silhouette edge, the weight is 0
                bool same_tri = edge.shape_id == shading_isect.shape_id &&
                    (edge.v0 == shading_isect.tri_id || edge.v1 == shading_isect.tri_id);
                if (edges_pmf[edge_id] > 0 &&
                        is_silhouette(shapes, shading_point.position, edge) &&
                        !same_tri) {
                    auto v0 = Vector3{get_v0(shapes, edge)};
                    auto v1 = Vector3{get_v1(shapes, edge)};
                    // If degenerate, the weight is 0
                    if (length_squared(v1 - v0) > 1e-10f) {
                        // Transform the vertices to local coordinates
                        auto v0o = m_inv * (v0 - shading_point.position);
                        auto v1o = m_inv * (v1 - shading_point.position);
                        // If below surface, the weight is 0
                        if (v0o[2] > 0.f || v1o[2] > 0.f) {
                            // Clip to the surface tangent plane
                            if (v0o[2] < 0.f) {
                                v0o = (v0o*v1o[2] - v1o*v0o[2]) / (v1o[2] - v0o[2]);
                            }
                            if (v1o[2] < 0.f) {
                                v1o = (v0o*v1o[2] - v1o*v0o[2]) / (v1o[2] - v0o[2]);
                            }
                            // Integrate over the edge using LTC
                            auto vodir = v1o - v0o;
                            auto wt = normalize(vodir);
                            auto l0 = dot(v0o, wt);
                            auto l1 = dot(v1o, wt);
                            auto vo = v0o - l0 * wt;
                            auto d = length(vo);
                            auto I = [&](Real l) {
                                return (l/(d*(d*d+l*l))+atan(l/d)/(d*d))*vo[2] +
                                       (l*l/(d*(d*d+l*l)))*wt[2];
                            };
                            auto Il0 = I(l0);
                            auto Il1 = I(l1);
                            edge_weights[sample_id] = max((Il1 - Il0) / edges_pmf[edge_id], Real(0));
                        }
                    }
                }
                if (sample_id == 0) {
                    resample_cdf[sample_id] = edge_weights[sample_id];
                } else { // sample_id > 0
                    resample_cdf[sample_id] = resample_cdf[sample_id - 1] + edge_weights[sample_id];
                }
            }
            if (resample_cdf[M - 1] <= 0) {
                return;
            }
            // Use resample_cdf to pick one edge
            auto resample_u = edge_sample.resample_sel * resample_cdf[M - 1];
            auto resample_id = -1;
            for (int sample_id = 0; sample_id < M; sample_id++) {
                if (resample_u <= resample_cdf[sample_id]) {
                    resample_id = sample_id;
                    break;
                }
            }
            if (edge_weights[resample_id] <= 0 || resample_id == -1) {
                // Just in case if there's some numerical error
                return;
            }
            edge_sample_weight = (resample_cdf[M - 1] / M) /
                (edge_weights[resample_id] * edges_pmf[edge_ids[resample_id]]);
            edge_id = edge_ids[resample_id];
        } else {
            // sample using a tree traversal
            auto pmf = Real(0);
            edge_id = sample_edge(edge_tree_roots,
                shading_point, m_inv, edge_sample.edge_sel, pmf);
            if (edge_id == -1) {
                return;
            }
            assert(pmf > 0);
            assert(edge_id >= 0);
            edge_sample_weight = 1 / pmf;
        }

        const auto &edge = edges[edge_id];
        if (!is_silhouette(shapes, shading_point.position, edge)) {
            return;
        }

        auto v0 = Vector3{get_v0(shapes, edge)};
        auto v1 = Vector3{get_v1(shapes, edge)};
        std::cerr << "v0:" << v0 << ", v1:" << v1 << std::endl;

        // Transform the vertices to local coordinates
        auto v0o = m_inv * (v0 - shading_point.position);
        auto v1o = m_inv * (v1 - shading_point.position);
        std::cerr << "v0o:" << v0o << ", v1o:" << v1o << std::endl;
        if (v0o[2] <= 0.f && v1o[2] <= 0.f) {
            // Edge is below the shading point
            return;
        }

        // Clip to the surface tangent plane
        if (v0o[2] < 0.f) {
            v0o = (v0o*v1o[2] - v1o*v0o[2]) / (v1o[2] - v0o[2]);
        }
        if (v1o[2] < 0.f) {
            v1o = (v0o*v1o[2] - v1o*v0o[2]) / (v1o[2] - v0o[2]);
        }
        auto vodir = v1o - v0o;
        std::cerr << "vodir:" << vodir << std::endl;
        auto wt = normalize(vodir);
        std::cerr << "wt:" << wt << std::endl;
        auto l0 = dot(v0o, wt);
        auto l1 = dot(v1o, wt);
        auto vo = v0o - l0 * wt;
        auto d = length(vo);
        auto I = [&](Real l) {
            return (l/(d*(d*d+l*l))+atan(l/d)/(d*d))*vo[2] +
                   (l*l/(d*(d*d+l*l)))*wt[2];
        };
        auto Il0 = I(l0);
        auto Il1 = I(l1);
        auto normalization = Il1 - Il0;
        auto line_cdf = [&](Real l) {
            return (I(l)-Il0)/normalization;
        };
        auto line_pdf = [&](Real l) {
            auto dist_sq=d*d+l*l;
            return 2.f*d*(vo+l*wt)[2]/(normalization*dist_sq*dist_sq);
        };
        // Hybrid bisection & Newton iteration
        // Here we are trying to find a point l s.t. line_cdf(l) = edge_sample.t
        auto lb = l0;
        auto ub = l1;
        if (lb > ub) {
            swap(lb, ub);
        }
        auto l = 0.5f * (lb + ub);
        for (int it = 0; it < 20; it++) {
            if (!(l >= lb && l <= ub)) {
                l = 0.5f * (lb + ub);
            }
            auto value = line_cdf(l) - edge_sample.t;
            if (fabs(value) < 1e-5f || it == 19) {
                break;
            }
            // The derivative may not be entirely accurate,
            // but the bisection is going to handle this
            if (value > 0.f) {
                ub = l;
            } else {
                lb = l;
            }
            auto derivative = line_pdf(l);
            l -= value / derivative;
        }
        std::cerr << "lb:" << lb << ", ub:" << ub << std::endl;
        std::cerr << "l:" << l << ", line_cdf(l):" << line_cdf(l) << ", line_pdf(l):" << line_pdf(l) << std::endl;
        if (line_pdf(l) <= 0.f) {
            // Numerical issue
            return;
        }
        // Convert from l to position
        auto sample_p = m * (vo + l * wt);

        // shading_point.position, v0 and v1 forms a half-plane
        // that splits the spaces into upper half-space and lower half-space
        auto half_plane_normal =
            normalize(cross(v0 - shading_point.position,
                            v1 - shading_point.position));
        // Generate sample directions
        auto offset = 1e-5f / length(sample_p);
        auto sample_dir = normalize(sample_p);
        // Sample two rays on the two sides of the edge
        auto v_upper_dir = normalize(sample_dir + offset * half_plane_normal);
        auto v_lower_dir = normalize(sample_dir - offset * half_plane_normal);

        auto eval_bsdf = bsdf(material, shading_point, wi, sample_dir, min_rough);
        if (sum(eval_bsdf) < 1e-6f) {
            return;
        }

        // Setup output
        auto nd = channel_info.num_total_dimensions;
        auto rd = channel_info.radiance_dimension;
        auto d_color = Vector3{
            d_rendered_image[nd * pixel_id + rd + 0],
            d_rendered_image[nd * pixel_id + rd + 1],
            d_rendered_image[nd * pixel_id + rd + 2]
        };
        edge_records[idx].edge = edge;
        edge_records[idx].edge_pt = sample_p; // for Jacobian computation 
        edge_records[idx].mwt = m * wt; // for Jacobian computation
        rays[2 * idx + 0] = Ray(shading_point.position, v_upper_dir, 1e-3f * length(sample_p));
        rays[2 * idx + 1] = Ray(shading_point.position, v_lower_dir, 1e-3f * length(sample_p));
        const auto &incoming_ray_differential = incoming_ray_differentials[pixel_id];
        // Propagate ray differentials
        auto bsdf_ray_differential = RayDifferential{};
        bsdf_ray_differential.org_dx = incoming_ray_differential.org_dx;
        bsdf_ray_differential.org_dy = incoming_ray_differential.org_dy;
        if (edge_sample.bsdf_component <= diffuse_pmf) {
            // HACK: Output direction has no dependencies w.r.t. input
            // However, since the diffuse BRDF serves as a low pass filter,
            // we want to assign a larger prefilter.
            bsdf_ray_differential.dir_dx = Vector3{0.03f, 0.03f, 0.03f};
            bsdf_ray_differential.dir_dy = Vector3{0.03f, 0.03f, 0.03f};
        } else {
            // HACK: we compute the half vector as the micronormal,
            // and use dndx/dndy to approximate the micronormal screen derivatives
            auto m = normalize(wi + sample_dir);
            auto m_local2 = dot(m, shading_point.shading_frame.n);
            auto dmdx = shading_point.dn_dx * m_local2;
            auto dmdy = shading_point.dn_dy * m_local2;
            auto dir_dx = incoming_ray_differential.dir_dx;
            auto dir_dy = incoming_ray_differential.dir_dy;
            // Igehy 1999, Equation 15
            auto ddotn_dx = dir_dx * m - wi * dmdx;
            auto ddotn_dy = dir_dy * m - wi * dmdy;
            // Igehy 1999, Equation 14
            bsdf_ray_differential.dir_dx =
                dir_dx - 2 * (-dot(wi, m) * shading_point.dn_dx + ddotn_dx * m);
            bsdf_ray_differential.dir_dy =
                dir_dy - 2 * (-dot(wi, m) * shading_point.dn_dy + ddotn_dy * m);
        }
        bsdf_differentials[2 * idx + 0] = bsdf_ray_differential;
        bsdf_differentials[2 * idx + 1] = bsdf_ray_differential;
        // edge_weight doesn't take the Jacobian between the shading point
        // and the ray intersection into account. We'll compute this later
        auto edge_weight = edge_sample_weight / (m_pmf * line_pdf(l));
        auto nt = throughput * eval_bsdf * d_color * edge_weight;
        // assert(isfinite(throughput));
        // assert(isfinite(eval_bsdf));
        // assert(isfinite(d_color));
        std::cerr << "edge_sample_weight:" << edge_sample_weight << std::endl;
        std::cerr << "m_pmf:" << m_pmf << std::endl;
        std::cerr << "line_pdf(l):" << line_pdf(l) << std::endl;
        assert(isfinite(edge_weight));
        new_throughputs[2 * idx + 0] = nt;
        new_throughputs[2 * idx + 1] = -nt;
    }

    const Shape *shapes;
    const Material *materials;
    const Edge *edges;
    int num_edges;
    const Vector3 cam_org;
    const Real *edges_pmf;
    const Real *edges_cdf;
    const EdgeTreeRoots edge_tree_roots;
    const int *active_pixels;
    const SecondaryEdgeSample *edge_samples;
    const Ray *incoming_rays;
    const RayDifferential *incoming_ray_differentials;
    const Intersection *shading_isects;
    const SurfacePoint *shading_points;
    const Vector3 *throughputs;
    const Real *min_roughness;
    const float *d_rendered_image;
    const ChannelInfo channel_info;
    SecondaryEdgeRecord *edge_records;
    Ray *rays;
    RayDifferential *bsdf_differentials;
    Vector3 *new_throughputs;
    Real *edge_min_roughness;
};

void sample_secondary_edges(const Scene &scene,
                            const BufferView<int> &active_pixels,
                            const BufferView<SecondaryEdgeSample> &samples,
                            const BufferView<Ray> &incoming_rays,
                            const BufferView<RayDifferential> &incoming_ray_differentials,
                            const BufferView<Intersection> &shading_isects,
                            const BufferView<SurfacePoint> &shading_points,
                            const BufferView<Vector3> &throughputs,
                            const BufferView<Real> &min_roughness,
                            const float *d_rendered_image,
                            const ChannelInfo &channel_info,
                            BufferView<SecondaryEdgeRecord> edge_records,
                            BufferView<Ray> rays,
                            BufferView<RayDifferential> &bsdf_differentials,
                            BufferView<Vector3> new_throughputs,
                            BufferView<Real> edge_min_roughness) {
    auto cam_org = xfm_point(scene.camera.cam_to_world, Vector3{0, 0, 0});
    parallel_for(secondary_edge_sampler{
        scene.shapes.data,
        scene.materials.data,
        scene.edge_sampler.edges.begin(),
        (int)scene.edge_sampler.edges.size(),
        cam_org,
        scene.edge_sampler.secondary_edges_pmf.begin(),
        scene.edge_sampler.secondary_edges_cdf.begin(),
        get_edge_tree_roots(scene.edge_sampler.edge_tree.get()),
        active_pixels.begin(),
        samples.begin(),
        incoming_rays.begin(),
        incoming_ray_differentials.begin(),
        shading_isects.begin(),
        shading_points.begin(),
        throughputs.begin(),
        min_roughness.begin(),
        d_rendered_image,
        channel_info,
        edge_records.begin(),
        rays.begin(),
        bsdf_differentials.begin(),
        new_throughputs.begin(),
        edge_min_roughness.begin()},
        active_pixels.size(), scene.use_gpu);
}

// The derivative of the intersection point w.r.t. a line parameter t
DEVICE
inline Vector3 intersect_jacobian(const Vector3 &org,
                                  const Vector3 &dir,
                                  const Vector3 &p,
                                  const Vector3 &n,
                                  const Vector3 &l) {
    // Jacobian of ray-plane intersection:
    // https://www.cs.princeton.edu/courses/archive/fall00/cs426/lectures/raycast/sld017.htm
    // d = -(p dot n)
    // t = -(org dot n + d) / (dir dot n)
    // p = org + t * dir
    // d p[i] / d dir[i] = t
    // d p[i] / d t = dir[i]
    // d t / d dir_dot_n = (org dot n - p dot n) / dir_dot_n^2
    // d dir_dot_n / d dir[j] = n[j]
    auto dir_dot_n = dot(dir, n);
    if (fabs(dir_dot_n) < 1e-10f) {
        return Vector3{0.f, 0.f, 0.f};
    }
    auto d = -dot(p, n);
    auto t = -(dot(org, n) + d) / dir_dot_n;
    if (t <= 0) {
        return Vector3{0.f, 0.f, 0.f};
    }
    return t * (l - dir * (dot(l, n) / dot(dir, n)));
}


struct secondary_edge_weights_updater {
    DEVICE void update_throughput(const Intersection &edge_isect,
                                  const SurfacePoint &edge_surface_point,
                                  const SurfacePoint &shading_point,
                                  const SecondaryEdgeRecord &edge_record,
                                  Vector3 &edge_throughput) {
        if (edge_isect.valid()) {
            // Hit a surface
            // Geometry term
            auto dir = edge_surface_point.position - shading_point.position;
            auto dist_sq = length_squared(dir);
            if (dist_sq < 1e-8f) {
                // Likely a self-intersection
                edge_throughput = Vector3{0, 0, 0};
                return;
            }

            auto n_dir = dir / sqrt(dist_sq);
            auto geometry_term = fabs(dot(edge_surface_point.geom_normal, n_dir)) / dist_sq;


            // Intersection Jacobian Jm(t) (Eq. 18 in the paper)
            auto isect_jacobian = intersect_jacobian(shading_point.position,
                                                     edge_record.edge_pt,
                                                     edge_surface_point.position,
                                                     edge_surface_point.geom_normal,
                                                     edge_record.mwt);
            // area of projection
            auto v0 = Vector3{get_v0(scene.shapes, edge_record.edge)};
            auto v1 = Vector3{get_v1(scene.shapes, edge_record.edge)};
            auto half_plane_normal = normalize(cross(v0 - shading_point.position,
                                                     v1 - shading_point.position));
            // ||Jm(t)|| / ||n_m x n_h|| in Eq. 15 in the paper
            auto line_jacobian = length(isect_jacobian) /
                length(cross(edge_surface_point.geom_normal, half_plane_normal)); 
            auto p = shading_point.position;
            auto d0 = v0 - p;
            auto d1 = v1 - p;
            auto dirac_jacobian = length(cross(d0, d1)); // Eq. 16 in the paper
            auto w = line_jacobian / dirac_jacobian;

            edge_throughput *= geometry_term * w;
            assert(isfinite(geometry_term));
            assert(isfinite(w));
        } else if (scene.envmap != nullptr) {
            // Hit an environment light
            auto p = shading_point.position;
            auto v0 = Vector3{get_v0(scene.shapes, edge_record.edge)};
            auto v1 = Vector3{get_v1(scene.shapes, edge_record.edge)};
            auto d0 = v0 - p;
            auto d1 = v1 - p;
            auto dirac_jacobian = length(cross(d0, d1)); // Eq. 16 in the paper
            // TODO: check the correctness of this
            auto line_jacobian = 1 / length_squared(edge_record.edge_pt - p);
            auto w = line_jacobian / dirac_jacobian;

            edge_throughput *= w;
        }
    }

    DEVICE void operator()(int idx) {
        auto pixel_id = active_pixels[idx];
        const auto &shading_point = shading_points[pixel_id];
        const auto &edge_isect0 = edge_isects[2 * idx + 0];
        const auto &edge_surface_point0 = edge_surface_points[2 * idx + 0];
        const auto &edge_isect1 = edge_isects[2 * idx + 1];
        const auto &edge_surface_point1 = edge_surface_points[2 * idx + 1];
        const auto &edge_record = edge_records[idx];
        if (edge_record.edge.shape_id < 0) {
            return;
        }

        update_throughput(edge_isect0,
                          edge_surface_point0,
                          shading_point,
                          edge_record,
                          edge_throughputs[2 * idx + 0]);
        update_throughput(edge_isect1,
                          edge_surface_point1,
                          shading_point,
                          edge_record,
                          edge_throughputs[2 * idx + 1]);
    }

    const FlattenScene scene;
    const int *active_pixels;
    const SurfacePoint *shading_points;
    const Intersection *edge_isects;
    const SurfacePoint *edge_surface_points;
    const SecondaryEdgeRecord *edge_records;
    Vector3 *edge_throughputs;
};

void update_secondary_edge_weights(const Scene &scene,
                                   const BufferView<int> &active_pixels,
                                   const BufferView<SurfacePoint> &shading_points,
                                   const BufferView<Intersection> &edge_isects,
                                   const BufferView<SurfacePoint> &edge_surface_points,
                                   const BufferView<SecondaryEdgeRecord> &edge_records,
                                   BufferView<Vector3> edge_throughputs) {
    parallel_for(secondary_edge_weights_updater{
        get_flatten_scene(scene),
        active_pixels.begin(),
        shading_points.begin(),
        edge_isects.begin(),
        edge_surface_points.begin(),
        edge_records.begin(),
        edge_throughputs.begin()},
        active_pixels.size(), scene.use_gpu);
}

struct secondary_edge_derivatives_accumulator {
    DEVICE void operator()(int idx) {
        auto pixel_id = active_pixels[idx];
        const auto &shading_point = shading_points[pixel_id];
        const auto &edge_record = edge_records[idx];
        d_vertices[2 * idx + 0] = DVertex{};
        d_vertices[2 * idx + 1] = DVertex{};
        if (edge_record.edge.shape_id < 0) {
            return;
        }

        auto edge_contrib0 = edge_contribs[2 * idx + 0];
        auto edge_contrib1 = edge_contribs[2 * idx + 1];
        const auto &edge_surface_point0 = edge_surface_points[2 * idx + 0];
        const auto &edge_surface_point1 = edge_surface_points[2 * idx + 1];

        auto dcolor_dp = Vector3{0, 0, 0};
        auto dcolor_dv0 = Vector3{0, 0, 0};
        auto dcolor_dv1 = Vector3{0, 0, 0};
        auto v0 = Vector3{get_v0(shapes, edge_record.edge)};
        auto v1 = Vector3{get_v1(shapes, edge_record.edge)};
        auto grad = [&](const Vector3 &p, const Vector3 &x, Real edge_contrib) {
            if (edge_contrib == 0) {
                return;
            }
            auto d0 = v0 - p;
            auto d1 = v1 - p;
            // Eq. 16 in the paper (see the errata)
            auto dp = cross(d1, d0) + cross(x - p, d1) + cross(d0, x - p);
            auto dv0 = cross(d1, x - p);
            auto dv1 = cross(x - p, d0);
            dcolor_dp += dp * edge_contrib;
            dcolor_dv0 += dv0 * edge_contrib;
            dcolor_dv1 += dv1 * edge_contrib;
        };
        grad(shading_point.position, edge_surface_point0, edge_contrib0);
        grad(shading_point.position, edge_surface_point1, edge_contrib1);
        assert(isfinite(edge_contrib0));
        assert(isfinite(edge_contrib1));
        assert(isfinite(dcolor_dp));

        d_points[pixel_id].position += dcolor_dp;
        d_vertices[2 * idx + 0].shape_id = edge_record.edge.shape_id;
        d_vertices[2 * idx + 0].vertex_id = edge_record.edge.v0;
        d_vertices[2 * idx + 0].d_v = dcolor_dv0;
        d_vertices[2 * idx + 1].shape_id = edge_record.edge.shape_id;
        d_vertices[2 * idx + 1].vertex_id = edge_record.edge.v1;
        d_vertices[2 * idx + 1].d_v = dcolor_dv1;
    }

    const Shape *shapes;
    const int *active_pixels;
    const SurfacePoint *shading_points;
    const SecondaryEdgeRecord *edge_records;
    const Vector3 *edge_surface_points;
    const Real *edge_contribs;
    SurfacePoint *d_points;
    DVertex *d_vertices;
};

void accumulate_secondary_edge_derivatives(const Scene &scene,
                                           const BufferView<int> &active_pixels,
                                           const BufferView<SurfacePoint> &shading_points,
                                           const BufferView<SecondaryEdgeRecord> &edge_records,
                                           const BufferView<Vector3> &edge_surface_points,
                                           const BufferView<Real> &edge_contribs,
                                           BufferView<SurfacePoint> d_points,
                                           BufferView<DVertex> d_vertices) {
    parallel_for(secondary_edge_derivatives_accumulator{
        scene.shapes.data,
        active_pixels.begin(),
        shading_points.begin(),
        edge_records.begin(),
        edge_surface_points.begin(),
        edge_contribs.begin(),
        d_points.begin(),
        d_vertices.begin()
    }, active_pixels.size(), scene.use_gpu);
}
