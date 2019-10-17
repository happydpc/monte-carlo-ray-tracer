#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <glm/vec3.hpp>
#include <glm/gtx/component_wise.hpp>

#include "Util.hpp"
#include "Scene.hpp"
#include "Octree.hpp"

struct Photon : public OctreeData
{
public:
    Photon(const glm::vec3& flux, const glm::vec3& position, const glm::vec3& direction)
        : flux(flux), position(position), direction(direction) { }

    // Shadow photon constructor
    Photon(const glm::vec3& position)
        : flux(0.0f), position(position), direction(0.0f) { }

    virtual glm::vec3 pos() const
    {
        return position;
    }

    glm::vec3 flux, position, direction;
};

class PhotonMap
{
public:
    PhotonMap(Scene& s, size_t photon_emissions, uint16_t max_node_data)
        : global(glm::vec3(5,0,0), glm::vec3(8, 5, 6) + glm::vec3(0.01f), max_node_data)
    {
        scene = std::make_shared<Scene>(s);

        double total_add_flux = 0.0;
        for (const auto& light : scene->emissives)
        {
            total_add_flux += glm::compAdd(light->material->emittance * light->area());
        }

        for (const auto& light : scene->emissives)
        {
            glm::dvec3 light_flux = light->material->emittance * light->area();
            double photon_emissions_share = glm::compAdd(light_flux) / total_add_flux;
            size_t num_light_emissions = static_cast<size_t>(photon_emissions * photon_emissions_share);
            glm::dvec3 photon_flux = light_flux / static_cast<double>(num_light_emissions);
            for (size_t i = 0; i < num_light_emissions; i++)
            {
                glm::dvec3 pos = (*light)(Random::range(0, 1), Random::range(0, 1));
                glm::dvec3 normal = light->normal(pos);
                glm::dvec3 dir = CoordinateSystem::localToGlobalUnitVector(Random::UniformHemiSample(), normal);

                pos += normal * 1e-7;

                emitPhoton(Ray(pos, pos + dir, scene->ior), photon_flux);
            }
        }
    }

    void emitPhoton(const Ray& ray, const glm::dvec3& flux, size_t ray_depth = 0)
    {
        if (ray_depth == max_ray_depth)
        {
            Log("Max photon ray depth reached.");
            return;
        }

        Intersection intersect = scene->intersect(ray, true);

        if (!intersect) return;

        // Use russian roulette regardless of material. Otherwise call stack 
        // overflow (or bias if prevented) is guaranteed with some scenes.
        double terminate = 1.0 - intersect.material->reflect_probability;
        bool should_terminate = terminate > Random::range(0, 1);

        Ray new_ray(intersect.position);

        glm::dvec3 BRDF;

        double n1 = ray.medium_ior;
        double n2 = abs(ray.medium_ior - scene->ior) < 1e-7 ? intersect.material->ior : scene->ior;

        if (intersect.material->perfect_mirror || Material::Fresnel(n1, n2, intersect.normal, -ray.direction) > Random::range(0, 1))
        {
            /* SPECULAR REFLECTION */
            if (ray_depth == 0 && Random::range(0, 1) < 0.1)
                createShadowPhotons(Ray(intersect.position - intersect.normal * 1e-7, intersect.position + ray.direction));

            if (should_terminate) return;

            BRDF = intersect.material->SpecularBRDF();
            new_ray.reflectSpecular(ray.direction, intersect.normal, n1);
        }
        else
        {
            if (intersect.material->transparency > Random::range(0, 1))
            {
                /* SPECULAR REFRACTION */
                if (should_terminate) return;

                // TODO: Maybe create shadow photons if reflection
                BRDF = intersect.material->SpecularBRDF();
                new_ray.refractSpecular(ray.direction, intersect.normal, n1, n2);
            }
            else
            {
                /* DIFFUSE REFLECTION */
                global.insert(Photon(flux, intersect.position, ray.direction));
                num_photons++;

                if (ray_depth == 0 && Random::range(0, 1) < 0.1)
                    createShadowPhotons(Ray(intersect.position - intersect.normal * 1e-7, intersect.position + ray.direction));

                if (should_terminate) return;

                CoordinateSystem cs(intersect.normal);
                BRDF = intersect.material->DiffuseBRDF(cs.globalToLocal(new_ray.direction), cs.globalToLocal(-ray.direction));
                new_ray.reflectDiffuse(cs, n1);
            }
        }
        emitPhoton(new_ray, flux * BRDF / (1.0 - terminate), ray_depth + 1);
    }

    void createShadowPhotons(const Ray& ray)
    {
        Intersection intersect = scene->intersect(ray, true);

        if (!intersect) return;

        // TODO: Might need to evaluate fresnel and transparency to probabilistically determine if shadow photon should be created
        if ((abs(intersect.material->transparency - 1.0) > 1e-7) && !intersect.material->perfect_mirror)
        {
            global.insert(Photon(intersect.position));
            num_shadow_photons++;
        }

        glm::dvec3 pos(intersect.position - intersect.normal * 1e-7);
        createShadowPhotons(Ray(pos, pos + ray.direction));
    }

    Octree<Photon> global;

    std::shared_ptr<Scene> scene;

    const size_t max_ray_depth = 64; // Prevent call stack overflow, unlikely to ever happen.

    size_t num_photons = 0;
    size_t num_shadow_photons = 0;
};