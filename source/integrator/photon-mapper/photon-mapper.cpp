#include "photon-mapper.hpp"

#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>

#include <glm/gtx/component_wise.hpp>

#include "../../random/random.hpp"
#include "../../common/util.hpp"
#include "../../common/work-queue.hpp"
#include "../../common/constants.hpp"
#include "../../common/format.hpp"

#include "../../octree/octree.cpp"
#include "../../octree/linear-octree.cpp"

PhotonMapper::PhotonMapper(const nlohmann::json& j) : Integrator(j)
{
    bool print = true;

    const nlohmann::json& pm = j.at("photon_map");

    double caustic_factor = pm.at("caustic_factor");
    size_t photon_emissions = pm.at("emissions");

    k_nearest_photons = getOptional(pm, "k_nearest_photons", 50);
    non_caustic_reject = 1.0 / caustic_factor;
    max_radius = pm.at("max_radius");
    max_caustic_radius = pm.at("max_caustic_radius");
    max_node_data = pm.at("max_photons_per_octree_leaf");
    direct_visualization = getOptional(pm, "direct_visualization", false);
    use_shadow_photons = getOptional(pm, "use_shadow_photons", true);
    
    BoundingBox BB = scene.BB();

    Octree<Photon> caustic_map(BB, max_node_data);
    Octree<Photon> direct_map(BB, max_node_data);
    Octree<Photon> indirect_map(BB, max_node_data);
    Octree<ShadowPhoton> shadow_map(BB, max_node_data);

    photon_emissions = static_cast<size_t>(photon_emissions * caustic_factor);

    const size_t EPW = 100000;

    double total_add_flux = 0.0;
    for (const auto& light : scene.emissives)
    {
        total_add_flux += glm::compAdd(light->material->emittance * light->area());
    }

    struct EmissionWork
    {
        EmissionWork() : light(), num_emissions(0), photon_flux(0.0) { }
        EmissionWork(std::shared_ptr<Surface::Base> light, size_t num_emissions, const glm::dvec3& photon_flux)
            : light(light), num_emissions(num_emissions), photon_flux(photon_flux) { }

        std::shared_ptr<Surface::Base> light;
        size_t num_emissions;
        glm::dvec3 photon_flux;
    };

    std::vector<EmissionWork> work_vec;

    for (const auto& light : scene.emissives)
    {
        glm::dvec3 light_flux = light->material->emittance * light->area();
        double photon_emissions_share = glm::compAdd(light_flux) / total_add_flux;
        size_t num_light_emissions = static_cast<size_t>(photon_emissions * photon_emissions_share);
        glm::dvec3 photon_flux = light_flux / static_cast<double>(num_light_emissions);

        size_t count = 0;
        while (count != num_light_emissions)
        {
            size_t emissions = count + EPW > num_light_emissions ? num_light_emissions - count : EPW;
            work_vec.emplace_back(light, emissions, photon_flux);
            count += emissions;
        }
    }

    std::shuffle(work_vec.begin(), work_vec.end(), Random::getEngine());
    WorkQueue<EmissionWork> work_queue(work_vec);

    std::vector<std::unique_ptr<std::thread>> threads(Integrator::num_threads);

    direct_vecs.resize(threads.size());
    indirect_vecs.resize(threads.size());
    caustic_vecs.resize(threads.size());
    shadow_vecs.resize(threads.size());

    for (size_t thread = 0; thread < threads.size(); thread++)
    {
        threads[thread] = std::make_unique<std::thread>
        (
            [this, &work_queue, thread]()
            {
                Random::seed(std::random_device{}()); // Each thread uses different seed.

                EmissionWork work;
                while (work_queue.getWork(work))
                {
                    for (size_t i = 0; i < work.num_emissions; i++)
                    {
                        glm::dvec3 pos = (*work.light)(Random::range(0, 1), Random::range(0, 1));
                        glm::dvec3 normal = work.light->normal(pos);
                        glm::dvec3 dir = CoordinateSystem::localToGlobal(Random::CosWeightedHemiSample(), normal);

                        pos += normal * C::EPSILON;

                        emitPhoton(Ray(pos, pos + dir, scene.ior), work.photon_flux, thread);
                    }
                }
            }
        );
    }

    auto begin = std::chrono::high_resolution_clock::now();
    std::unique_ptr<std::thread> print_thread;
    if (print)
    {
        std::cout << std::string(28, '-') << "| PHOTON MAPPING PASS |" << std::string(28, '-') << std::endl << std::endl;
        std::cout << "Total number of photon emissions from light sources: " << Format::largeNumber(photon_emissions) << std::endl << std::endl;
        print_thread = std::make_unique<std::thread>([&work_queue]()
        {
            while (!work_queue.empty())
            {
                double progress = work_queue.progress();
                std::cout << std::string("\rPhotons emitted: " + Format::progress(progress));
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        });
        print_thread->join();
    }

    for (auto& thread : threads)
    {
        thread->join();
    }

    std::atomic<bool> done_constructing_octrees = false;
    auto end = std::chrono::high_resolution_clock::now();
    std::string duration = Format::timeDuration(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    if (print)
    {
        std::string info = "\rPhotons emitted in " + duration + ". Constructing octrees";
        std::cout << info;
        begin = std::chrono::high_resolution_clock::now();
            
        print_thread = std::make_unique<std::thread>([&done_constructing_octrees, info]()
        {
            std::string dots("");
            int i = 0;
            while (!done_constructing_octrees)
            {
                std::cout << "\r" + std::string(60, ' ') + info + dots;
                dots += ".";
                if (i != 0 && i % 3 == 0) dots = ".";
                i++;
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }
        });
    }
        
    size_t num_direct_photons = 0;
    size_t num_indirect_photons = 0;
    size_t num_caustic_photons = 0;
    size_t num_shadow_photons = 0;

    // Erase elements from the vectors as they are inserted in the octree, 
    // otherwise more memory than needed is used momentarily.
    auto insertAndPop = [](auto& pvec, auto& pmap)
    {
        auto i = pvec.end();
        while (i > pvec.begin())
        {
            i--;
            pmap.insert(*i);
            i = pvec.erase(i);
        }
        pvec.clear();
    };

    for (size_t thread = 0; thread < threads.size(); thread++)
    {
        num_direct_photons += direct_vecs[thread].size();
        insertAndPop(direct_vecs[thread], direct_map);

        num_indirect_photons += indirect_vecs[thread].size();
        insertAndPop(indirect_vecs[thread], indirect_map);

        num_caustic_photons += caustic_vecs[thread].size();
        insertAndPop(caustic_vecs[thread], caustic_map);

        num_shadow_photons += shadow_vecs[thread].size();
        insertAndPop(shadow_vecs[thread], shadow_map);
    }

    // Convert octrees to linear array representation
    linear_caustic_map  = LinearOctree<Photon>(caustic_map);
    linear_direct_map   = LinearOctree<Photon>(direct_map);
    linear_indirect_map = LinearOctree<Photon>(indirect_map);
    linear_shadow_map   = LinearOctree<ShadowPhoton>(shadow_map);

    done_constructing_octrees = true;

    if (print)
    {
        print_thread->join();
        end = std::chrono::high_resolution_clock::now();
        std::string duration2 = Format::timeDuration(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
        std::cout << "\rPhotons emitted in " + duration + ". Octrees constructed in " + duration2 + "." << std::endl << std::endl
                  << "Photon maps and numbers of stored photons: " << std::endl << std::endl;

        std::cout << std::right
                  << std::setw(19) << "Direct photons: "   << Format::largeNumber(num_direct_photons)   << std::endl
                  << std::setw(19) << "Indirect photons: " << Format::largeNumber(num_indirect_photons) << std::endl
                  << std::setw(19) << "Caustic photons: "  << Format::largeNumber(num_caustic_photons)  << std::endl
                  << std::setw(19) << "Shadow photons: "   << Format::largeNumber(num_shadow_photons)   << std::endl << std::endl;
    }
}

void PhotonMapper::emitPhoton(const Ray& ray, const glm::dvec3& flux, size_t thread, size_t ray_depth)
{
    if (ray_depth == Integrator::max_ray_depth)
    {
        Log("Bias introduced: Max ray depth reached in PhotonMap::emitPhoton()");
        return;
    }

    Intersection intersect = scene.intersect(ray);

    if (!intersect)
    {
        return;
    }

    Ray new_ray(intersect.position);

    glm::dvec3 BRDF;

    double n1 = ray.medium_ior;
    double n2 = std::abs(ray.medium_ior - scene.ior) < C::EPSILON ? intersect.material->ior : scene.ior;

    switch (intersect.selectNewPath(n1, n2, -ray.direction))
    {
        case Path::REFLECT:
        {
            if (ray_depth == 0 && Random::trial(non_caustic_reject))
            {
                createShadowPhotons(Ray(intersect.position - intersect.normal * C::EPSILON, intersect.position + ray.direction), thread);
            }

            BRDF = intersect.material->SpecularBRDF();
            new_ray.reflectSpecular(ray.direction, intersect, n1);
            break;
        }
        case Path::REFRACT:
        {
            BRDF = intersect.material->SpecularBRDF();
            new_ray.refractSpecular(ray.direction, intersect, n1, n2);
            break;
        }
        case Path::DIFFUSE:
        {
            if (ray_depth == 0)
            { 
                if (Random::trial(non_caustic_reject))
                {
                    direct_vecs[thread].emplace_back(flux / non_caustic_reject, intersect.position, ray.direction);
                    createShadowPhotons(Ray(intersect.position - intersect.normal * C::EPSILON, intersect.position + ray.direction), thread);
                }
            }
            else
            {
                if (ray.specular)
                {
                    caustic_vecs[thread].emplace_back(flux, intersect.position, ray.direction);
                }
                else if(Random::trial(non_caustic_reject))
                {
                    indirect_vecs[thread].emplace_back(flux / non_caustic_reject, intersect.position, ray.direction);
                }
            }
            
            CoordinateSystem cs(intersect.shadingNormal());
            new_ray.reflectDiffuse(cs, intersect, n1);

            BRDF = C::PI * intersect.material->DiffuseBRDF(cs.globalToLocal(new_ray.direction), cs.globalToLocal(-ray.direction));
        }
    }

    glm::dvec3 new_flux = flux * BRDF;

    // Based on slide 13 of https://cgg.mff.cuni.cz/~jaroslav/teaching/2015-npgr010/slides/11%20-%20npgr010-2015%20-%20PM.pdf
    double survive = std::min(ray_depth > min_ray_depth ? 0.9 : 1.0, glm::compMax(new_flux) / glm::compMax(flux));

    if (Random::trial(survive))
    {
        emitPhoton(new_ray, new_flux / survive, thread, ray_depth + 1);
    }

    return;
}

void PhotonMapper::createShadowPhotons(const Ray& ray, size_t thread, size_t depth)
{
    if (!use_shadow_photons)
    {
        return;
    }

    if (depth > max_ray_depth)
    {
        return;
    }

    Intersection intersect = scene.intersect(ray);

    if (!intersect)
    {
        return;
    }

    if (intersect.material->can_diffusely_reflect)
    {
        shadow_vecs[thread].emplace_back(intersect.position);
    }

    glm::dvec3 pos(intersect.position - intersect.normal * C::EPSILON);
    createShadowPhotons(Ray(pos, pos + ray.direction), thread, depth + 1);
}

glm::dvec3 PhotonMapper::sampleRay(Ray ray, size_t ray_depth)
{
    if (ray_depth == Integrator::max_ray_depth)
    {
        Log("Bias introduced: Max ray depth reached in PhotonMap::sampleRay()");
        return glm::dvec3(0.0);
    }

    Intersection intersect = scene.intersect(ray);

    if (!intersect)
    {
        return scene.skyColor(ray);
    }

    double absorb = ray_depth > min_ray_depth ? 1.0 - intersect.material->reflect_probability : 0.0;
    
    if (Random::trial(absorb))
    {
        return glm::dvec3(0.0);
    }

    Ray new_ray(intersect.position);

    bool diffuse = ray_depth != 0 && !ray.specular;

    bool global_contribution_evaluated = false;
    glm::dvec3 caustics(0.0), direct(0.0), indirect(0.0), BRDF(1.0);
    glm::dvec3 emittance = (ray_depth == 0 || ray.specular) ? intersect.material->emittance : glm::dvec3(0.0);

    double n1 = ray.medium_ior;
    double n2 = std::abs(ray.medium_ior - scene.ior) < C::EPSILON ? intersect.material->ior : scene.ior;

    switch (intersect.selectNewPath(n1, n2, -ray.direction))
    {
        case Path::REFLECT:
        {
            if (diffuse) return glm::dvec3(0.0);
            BRDF = intersect.material->SpecularBRDF();
            new_ray.reflectSpecular(ray.direction, intersect, n1);
            break;
        }
        case Path::REFRACT:
        {
            if (diffuse) return glm::dvec3(0.0);
            BRDF = intersect.material->SpecularBRDF();
            new_ray.refractSpecular(ray.direction, intersect, n1, n2);
            break;
        }
        case Path::DIFFUSE:
        {
            CoordinateSystem cs(intersect.shadingNormal());

            caustics = estimateCausticRadiance(intersect, -ray.direction, cs);

            bool has_shadow_photons = hasShadowPhoton(intersect);
            if (!direct_visualization && (ray_depth == 0 || ray.specular || has_shadow_photons))
            {
                double n1 = ray.medium_ior;
                new_ray.reflectDiffuse(cs, intersect, n1);
                BRDF = intersect.material->DiffuseBRDF(cs.globalToLocal(new_ray.direction), cs.globalToLocal(-ray.direction)) * C::PI;
                direct = sampleDirect(intersect);
            }
            else
            {
                direct = estimateRadiance(linear_direct_map, intersect, -ray.direction, cs, max_radius);
                indirect = estimateRadiance(linear_indirect_map, intersect, -ray.direction, cs, max_radius);
                global_contribution_evaluated = true;
            }
            break;
        }
    }

    // Terminate the path once the global contribution has been evaluated to estimate all incoming radiance at this point. This happens 
    // once a diffusely reflected ray hits a point that evaluates to diffuse, provided that there are no shadow photons at this point.
    if (!global_contribution_evaluated)
    {
        indirect = sampleRay(new_ray, ray_depth + 1);
    }

    // BRDF is 1 if the indirect and direct radiance was estimated from the photon maps.
    return (emittance + caustics + (indirect + direct) * BRDF) / (1.0 - absorb);
}

glm::dvec3 PhotonMapper::estimateRadiance(LinearOctree<Photon>& map, const Intersection& intersect,
                                       const glm::dvec3& direction, const CoordinateSystem& cs, double r) const
{
    glm::dvec3 radiance(0.0);
    auto photons = map.knnSearch(intersect.position, k_nearest_photons, r);
    if (photons.empty()) return radiance;

    for (const auto& p : photons)
    {
        if (glm::dot(p.data.direction, cs.normal) >= 0.0) continue;
        glm::dvec3 BRDF = intersect.material->DiffuseBRDF(cs.globalToLocal(p.data.direction), cs.globalToLocal(direction));
        radiance += p.data.flux * BRDF;
    }
    return radiance / photons.back().distance2;
}

/*********************************************************************************
Cone filtering method that can be used for sharper caustics. Simplified for k = 1
**********************************************************************************/
glm::dvec3 PhotonMapper::estimateCausticRadiance(const Intersection& intersect, const glm::dvec3& direction, const CoordinateSystem& cs)
{
    glm::dvec3 radiance(0.0);
    auto photons = linear_caustic_map.knnSearch(intersect.position, k_nearest_photons, max_caustic_radius);
    if (photons.empty()) return radiance;

    double max_squared_radius = photons.back().distance2;

    for (const auto& p : photons)
    {
        if (glm::dot(p.data.direction, cs.normal) >= 0.0) continue;
        double wp = std::max(0.0, 1.0 - std::sqrt(p.distance2 / max_squared_radius));
        glm::dvec3 BRDF = intersect.material->DiffuseBRDF(cs.globalToLocal(p.data.direction), cs.globalToLocal(direction));
        radiance += p.data.flux * BRDF * wp;
    }
    return radiance / (max_squared_radius / 3.0);
}

bool PhotonMapper::hasShadowPhoton(const Intersection& intersect) const
{
    return use_shadow_photons && !linear_shadow_map.knnSearch(intersect.position, k_nearest_photons, max_radius).empty();
}
